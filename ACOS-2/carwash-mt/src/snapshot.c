/**
 * @file snapshot.c
 * @brief Снимок линии: стадии по одной под своими мьютексами, затем счётчики.
 */

#include "snapshot.h"

#include <string.h>

#include "sim.h"

void SnapshotTake(Line *l, Snapshot *s) {
    const Config *cfg = l->cfg;
    memset(s, 0, sizeof *s);
    s->now = SimNow();
    for (int i = 0; i < cfg->stages; ++i) {
        Stage *st = &l->stage[i];
        MutexLock(&st->m);
        s->queued[i] = st->queued;
        memcpy(s->queue[i], st->queue, sizeof(s->queue[i]));
        for (int p = 0; p < cfg->stage[i].posts; ++p) {
            memcpy(s->seat[i][p], st->post[p].seat, sizeof(s->seat[i][p]));
            s->repair[i][p] = st->post[p].repair;
        }
        MutexUnlock(&st->m);
    }
    MutexLock(&l->counters);
    s->open = l->open;
    s->inside = l->inside;
    s->served = l->served;
    s->rejected = l->rejected;
    MutexUnlock(&l->counters);
    MutexLock(&l->registry);
    s->arrived = l->carCount;
    MutexUnlock(&l->registry);
}
