/**
 * @file quiesce.c
 * @brief Крючок момента покоя: конец моделирования и нехватка ресурсов.
 *
 * В покое никто из потоков модели не работает, поэтому счётчики линии устойчивы.
 */

#include "quiesce.h"

#include "line.h"

int QuiesceHook(int now, int nextWake, void *line) {
    (void)now;
    (void)nextWake;
    Line *l = line;
    if (!atomic_load(&l->open) && atomic_load(&l->inside) == 0) {
        l->status = LINE_FINISHED;
        return 0;
    }
    if (atomic_load(&l->requests) & REQ_NO_RESOURCES) {
        l->status = LINE_NO_RESOURCES;
        return 0;
    }
    return 1;
}
