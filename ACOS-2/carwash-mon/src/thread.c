#include "thread.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>

static atomic_long attempts = 0;

static int HookFails(long attempt) {
    // тестовый крючок; окружение меняют только тесты, когда других потоков нет
    const char *env = getenv(  // NOLINT(concurrency-mt-unsafe)
        "CARWASH_FAIL_THREAD_AFTER");
    if (!env || !*env) {
        return 0;
    }
    char *end = NULL;
    long after = strtol(env, &end, 10);
    return *end == '\0' && attempt >= after;
}

int ThreadCreate(pthread_t *t, void *(*fn)(void *), void *arg, size_t stack) {
    long attempt = atomic_fetch_add(&attempts, 1);
    if (HookFails(attempt)) {
        return EAGAIN;
    }
    if (stack == 0) {
        return pthread_create(t, NULL, fn, arg);
    }
    pthread_attr_t attr;
    int rc = pthread_attr_init(&attr);
    if (rc != 0) {
        return rc;
    }
    rc = pthread_attr_setstacksize(&attr, stack);
    if (rc == 0) {
        rc = pthread_create(t, &attr, fn, arg);
    }
    pthread_attr_destroy(&attr);
    return rc;
}
