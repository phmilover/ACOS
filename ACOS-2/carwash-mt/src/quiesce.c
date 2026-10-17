/**
 * @file quiesce.c
 * @brief Крючок момента покоя: конец моделирования и нехватка ресурсов.
 */

#include "quiesce.h"

#include "line.h"

int QuiesceHook(int now, int nextWake, void *line) {
    (void)now;
    (void)nextWake;
    Line *l = line;
    MutexLock(&l->counters);
    int finished = !l->open && l->inside == 0;
    MutexUnlock(&l->counters);
    if (finished) {
        l->status = LINE_FINISHED;
        return 0;
    }
    if (atomic_load(&l->requests) & REQ_NO_RESOURCES) {
        l->status = LINE_NO_RESOURCES;
        return 0;
    }
    return 1;
}
