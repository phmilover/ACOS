#include "sync.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define LOCKDEP_DEPTH 16

/// Запись стека LOCKDEP: достаточно ранга, имени и адреса замка для снятия.
typedef struct {
    const void *lock;
    int rank;
    const char *name;
} Held;

static _Thread_local Held heldStack[LOCKDEP_DEPTH];
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

_Noreturn void FatalExit(int code) {
    void (*hook)(void) = atomic_load(&exitHook);
    if (hook) {
        hook();
    }
    _exit(code);
}

/// Сообщает о нарушении: одним write в fd 2 (строки разных потоков не перемешаются).
static void Report(const char *name, int rank, const Held *h) {
    atomic_fetch_add(&violations, 1);
    char buf[256];
    int n = snprintf(buf, sizeof buf,
                     "LOCKDEP: thread %s takes %s (rank %d) while holding %s (rank %d)\n",
                     threadName, name, rank, h->name, h->rank);
    if (n > (int)sizeof buf - 1) {
        n = (int)sizeof buf - 1;
    }
    if (n > 0 && write(2, buf, (size_t)n) < 0) {
        // писать некуда, дальше падать нечем
    }
    if (atomic_load(&fatalMode)) {
        FatalExit(4);
    }
}

/// Проверяет порядок рангов до блокировки.
static void CheckOrder(const char *name, int rank) {
    const Held *top = NULL;  // удерживаемый замок с наибольшим рангом
    for (int i = 0; i < heldCount; ++i) {
        if (!top || heldStack[i].rank > top->rank) {
            top = &heldStack[i];
        }
    }
    if (top && rank <= top->rank) {
        Report(name, rank, top);
    }
}

static void Push(const void *lock, int rank, const char *name) {
    if (heldCount < LOCKDEP_DEPTH) {
        heldStack[heldCount++] = (Held){lock, rank, name};
    }
}

static void Pop(const void *lock) {
    for (int i = heldCount - 1; i >= 0; --i) {
        if (heldStack[i].lock == lock) {
            for (int j = i; j + 1 < heldCount; ++j) {
                heldStack[j] = heldStack[j + 1];
            }
            --heldCount;
            break;
        }
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
    CheckOrder(m->name, m->rank);
    pthread_mutex_lock(&m->raw);
    Push(m, m->rank, m->name);
}

NO_TSA void MutexUnlock(Mutex *m) {
    Pop(m);
    pthread_mutex_unlock(&m->raw);
}

int CondInit(Cond *c) {
    return pthread_cond_init(&c->raw, NULL) == 0 ? 0 : -1;
}

void CondDestroy(Cond *c) {
    pthread_cond_destroy(&c->raw);
}

void CondWait(Cond *c, Mutex *m) {
    pthread_cond_wait(&c->raw, &m->raw);
}

int CondTimedWaitMs(Cond *c, Mutex *m, int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_nsec -= 1000000000L;
        ts.tv_sec += 1;
    }
    return pthread_cond_timedwait(&c->raw, &m->raw, &ts) == ETIMEDOUT ? 1 : 0;
}

void CondSignal(Cond *c) {
    pthread_cond_signal(&c->raw);
}

void CondBroadcast(Cond *c) {
    pthread_cond_broadcast(&c->raw);
}

int RwLockInit(RwLock *l, int rank, const char *name) {
    l->rank = rank;
    l->name = name;
    return pthread_rwlock_init(&l->raw, NULL) == 0 ? 0 : -1;
}

void RwLockDestroy(RwLock *l) {
    pthread_rwlock_destroy(&l->raw);
}

NO_TSA void RwLockRead(RwLock *l) {
    CheckOrder(l->name, l->rank);
    pthread_rwlock_rdlock(&l->raw);
    Push(l, l->rank, l->name);
}

NO_TSA void RwLockWrite(RwLock *l) {
    CheckOrder(l->name, l->rank);
    pthread_rwlock_wrlock(&l->raw);
    Push(l, l->rank, l->name);
}

NO_TSA void RwLockUnlock(RwLock *l) {
    Pop(l);
    pthread_rwlock_unlock(&l->raw);
}
