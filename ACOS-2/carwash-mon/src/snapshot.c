/**
 * @file snapshot.c
 * @brief Снимок линии: стадии по одной под своими мьютексами, затем счётчики (атомики).
 */

#include "snapshot.h"

#include <string.h>

#include "sim.h"

/// Строка панели: сначала машины с WAIT_ROOM, ждущие в очереди занимают только остаток.
static void AddPanelCar(Snapshot *s, const SimThread *t) {
    PanelCar c = {.car = t->id, .why = t->why.kind, .stage = t->why.stage, .sysId = t->sysId};
    if (s->panelCars < PANEL_CARS) {
        s->panel[s->panelCars++] = c;
        return;
    }
    if (c.why != WAIT_ROOM) {
        return;
    }
    for (int i = 0; i < PANEL_CARS; ++i) {
        if (s->panel[i].why == WAIT_QUEUE) {
            s->panel[i] = c;
            return;
        }
    }
}

/// Под мьютексом часов: только копирование в снимок, без Sim* и без других мьютексов.
static void CollectThread(const SimThread *t, void *ctx) {
    Snapshot *s = ctx;
    if (t->state == ST_DONE) {
        return;
    }
    PanelPhase ph = PH_OTHER;
    if (t->state == ST_SLEEPING) {
        ph = PH_SLEEP;
    } else if (t->state == ST_PARKED) {
        switch (t->why.kind) {
        case WAIT_OPERATION: ph = PH_WASH; break;
        case WAIT_QUEUE: ph = PH_QUEUE; break;
        case WAIT_ROOM: ph = PH_ROOM; break;
        case WAIT_REPAIR: ph = PH_REPAIR; break;
        case WAIT_NONE:
        case WAIT_TIME:
        default: break;
        }
    }
    s->threads[t->kind]++;
    s->phase[t->kind][ph]++;
    if (t->kind == TH_CAR && (ph == PH_ROOM || ph == PH_QUEUE)) {
        AddPanelCar(s, t);
    }
}

/// Панель по возрастанию номера машины (их не больше PANEL_CARS).
static void SortPanel(Snapshot *s) {
    for (int i = 1; i < s->panelCars; ++i) {
        PanelCar c = s->panel[i];
        int j = i;
        for (; j > 0 && s->panel[j - 1].car > c.car; --j) {
            s->panel[j] = s->panel[j - 1];
        }
        s->panel[j] = c;
    }
}

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
    s->open = atomic_load(&l->open);
    s->inside = atomic_load(&l->inside);
    s->served = atomic_load(&l->served);
    s->rejected = atomic_load(&l->rejected);
    s->failures = atomic_load(&l->failures);
    s->status = l->status;
    // программы машин (цвет в живой схеме): реестр берётся после стадий, не вложенно
    RwLockRead(&l->registry);
    s->arrived = l->carCount;
    RwLockUnlock(&l->registry);
    for (int i = 0; i < cfg->stages; ++i) {
        for (int q = 0; q < s->queued[i]; ++q) {
            const Car *c = LineCar(l, s->queue[i][q]);
            s->queueProg[i][q] = c ? c->program : 0;
        }
        for (int p = 0; p < cfg->stage[i].posts; ++p) {
            for (int k = 0; k < MAX_CAPACITY; ++k) {
                const Car *c = s->seat[i][p][k].car >= 0 ? LineCar(l, s->seat[i][p][k].car) : NULL;
                s->seatProg[i][p][k] = c ? c->program : 0;
            }
        }
    }
    SimForEach(CollectThread, s);
    s->parks = SimTotalParks();
    SortPanel(s);
}
