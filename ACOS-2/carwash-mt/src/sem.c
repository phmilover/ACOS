#include "sem.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

// TSan на macOS не видит happens-before между sem_post и sem_wait именованного семафора и
// сообщает о ложных гонках. В сборке с TSan передаём ему эту связь вручную.
#ifdef __has_feature
#if __has_feature(thread_sanitizer)
#define CW_TSAN 1
#endif
#endif
#if !defined(CW_TSAN) && defined(__SANITIZE_THREAD__)
#define CW_TSAN 1
#endif

#ifdef CW_TSAN
// имена задаёт рантайм TSan
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
void __tsan_acquire(void *addr);
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
void __tsan_release(void *addr);
#define TSAN_ACQUIRE(p) __tsan_acquire(p)
#define TSAN_RELEASE(p) __tsan_release(p)
#else
#define TSAN_ACQUIRE(p) ((void)(p))
#define TSAN_RELEASE(p) ((void)(p))
#endif

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
    while (sem_wait(s->h) != 0) {
        if (errno != EINTR) {
            return;
        }
    }
    TSAN_ACQUIRE(s->h);
}

int SemTryWait(Sem *s) {
    if (sem_trywait(s->h) != 0) {
        return -1;
    }
    TSAN_ACQUIRE(s->h);
    return 0;
}

void SemPost(Sem *s) {
    TSAN_RELEASE(s->h);
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
