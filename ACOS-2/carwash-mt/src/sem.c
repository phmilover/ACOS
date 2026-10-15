#include "sem.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static atomic_long nameCounter = 0;  // номер для уникального имени
static atomic_long attempts = 0;     // сколько раз звали SemOpen (для крючка)
static atomic_long openCount = 0;

/// Крючок отказа: true, если этот вызов должен упасть. Переменная читается каждый раз.
static int HookFails(long attempt) {
    // тестовый крючок; окружение меняют только тесты, когда других потоков нет
    const char *env = getenv(  // NOLINT(concurrency-mt-unsafe)
        "CARWASH_FAIL_SEM_AFTER");
    if (!env || !*env) {
        return 0;
    }
    char *end = NULL;
    long after = strtol(env, &end, 10);
    return *end == '\0' && attempt >= after;
}

int SemOpen(Sem *s, unsigned value) {
    long attempt = atomic_fetch_add(&attempts, 1);
    if (HookFails(attempt)) {
        errno = ENOSPC;
        return -1;
    }
    // имя короткое: на macOS предел около 31 символа
    char name[32];
    snprintf(name, sizeof name, "/cwmt-%d-%ld", (int)getpid(), atomic_fetch_add(&nameCounter, 1));
    sem_t *h = sem_open(name, O_CREAT | O_EXCL, 0600, value);
    if (h == SEM_FAILED) {
        return -1;
    }
    sem_unlink(name);
    s->h = h;
    atomic_fetch_add(&openCount, 1);
    return 0;
}

void SemWait(Sem *s) {
    while (sem_wait(s->h) != 0 && errno == EINTR) {
    }
}

int SemTryWait(Sem *s) {
    return sem_trywait(s->h) == 0 ? 0 : -1;
}

void SemPost(Sem *s) {
    sem_post(s->h);
}

void SemClose(Sem *s) {
    if (s->h) {
        sem_close(s->h);
        s->h = NULL;
        atomic_fetch_sub(&openCount, 1);
    }
}

long SemOpenCount(void) {
    return atomic_load(&openCount);
}
