#include "mutex.h"

#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

#define LOCKDEP_DEPTH 16

static _Thread_local Mutex *heldStack[LOCKDEP_DEPTH];
static _Thread_local int heldCount = 0;
static _Thread_local const char *threadName = "?";

static atomic_long violations = 0;
static atomic_int fatalMode = 1;
static _Atomic(void (*)(void)) exitHook = NULL;

void LockdepSetExitHook(void (*hook)(void)) {
    atomic_store(&exitHook, hook);
}

void LockdepSetThreadName(const char *name) {
    threadName = name ? name : "?";
}

int LockdepHeldCount(void) {
    return heldCount;
}

long LockdepViolations(void) {
    return atomic_load(&violations);
}

void LockdepSetFatal(int fatal) {
    atomic_store(&fatalMode, fatal);
}

/// Сообщает о нарушении: одним write в fd 2 (строки разных потоков не перемешаются).
static void Report(const Mutex *m, const Mutex *h) {
    atomic_fetch_add(&violations, 1);
    char buf[256];
    int n = snprintf(buf, sizeof buf,
                     "LOCKDEP: thread %s takes %s (rank %d) while holding %s (rank %d)\n",
                     threadName, m->name, m->rank, h->name, h->rank);
    if (n > (int)sizeof buf - 1) {
        n = (int)sizeof buf - 1;
    }
    if (n > 0 && write(2, buf, (size_t)n) < 0) {
        // писать некуда, дальше падать нечем
    }
    if (atomic_load(&fatalMode)) {
        void (*hook)(void) = atomic_load(&exitHook);
        if (hook) {
            hook();
        }
        _exit(4);
    }
}

void MutexInit(Mutex *m, int rank, const char *name) {
    pthread_mutex_init(&m->raw, NULL);
    m->rank = rank;
    m->name = name;
}

void MutexDestroy(Mutex *m) {
    pthread_mutex_destroy(&m->raw);
}

NO_TSA void MutexLock(Mutex *m) {
    const Mutex *top = NULL;  // удерживаемый мьютекс с наибольшим рангом
    for (int i = 0; i < heldCount; ++i) {
        if (!top || heldStack[i]->rank > top->rank) {
            top = heldStack[i];
        }
    }
    if (top && m->rank <= top->rank) {
        Report(m, top);
    }
    pthread_mutex_lock(&m->raw);
    if (heldCount < LOCKDEP_DEPTH) {
        heldStack[heldCount++] = m;
    }
}

NO_TSA void MutexUnlock(Mutex *m) {
    for (int i = heldCount - 1; i >= 0; --i) {
        if (heldStack[i] == m) {
            for (int j = i; j + 1 < heldCount; ++j) {
                heldStack[j] = heldStack[j + 1];
            }
            --heldCount;
            break;
        }
    }
    pthread_mutex_unlock(&m->raw);
}
