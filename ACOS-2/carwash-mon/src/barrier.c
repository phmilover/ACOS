#include "barrier.h"

#ifdef BARRIER_NATIVE

int BarrierInit(Barrier *b, unsigned count) {
    if (count < 1) {
        return -1;
    }
    return pthread_barrier_init(&b->raw, NULL, count) == 0 ? 0 : -1;
}

int BarrierWait(Barrier *b) {
    return pthread_barrier_wait(&b->raw) == PTHREAD_BARRIER_SERIAL_THREAD ? 1 : 0;
}

void BarrierDestroy(Barrier *b) {
    pthread_barrier_destroy(&b->raw);
}

const char *BarrierKind(void) {
    return "pthread_barrier";
}

#else

int BarrierInit(Barrier *b, unsigned count) {
    if (count < 1) {
        return -1;
    }
    MutexInit(&b->m, RANK_OUT, "barrier");
    if (CondInit(&b->cv) != 0) {
        MutexDestroy(&b->m);
        return -1;
    }
    b->count = count;
    MutexLock(&b->m);  // поля ещё никто не видит; захват нужен анализатору потокобезопасности
    b->arrived = 0;
    b->generation = 0;
    MutexUnlock(&b->m);
    return 0;
}

int BarrierWait(Barrier *b) {
    MutexLock(&b->m);
    unsigned long gen = b->generation;
    int last = 0;
    if (++b->arrived == b->count) {
        b->arrived = 0;
        ++b->generation;
        CondBroadcast(&b->cv);
        last = 1;
    } else {
        while (gen == b->generation) {
            CondWait(&b->cv, &b->m);
        }
    }
    MutexUnlock(&b->m);
    return last;
}

void BarrierDestroy(Barrier *b) {
    CondDestroy(&b->cv);
    MutexDestroy(&b->m);
}

const char *BarrierKind(void) {
    return "mutex+cond";
}

#endif
