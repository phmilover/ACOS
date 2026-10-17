/**
 * @file line.c
 * @brief Структуры линии: создание, освобождение и запросы в момент покоя.
 *
 * Запросы LineProgressPossible / LineEntranceMayAccept / LineStallCause - перенос логики
 * AnyProgressPossible / EntranceMayAccept / FindStallCause из ИЗ1 на места постов. Зовутся
 * только в момент покоя (никто не работает), но мьютексы стадий всё равно берут - по одному,
 * не вкладывая: так анализатор и TSan видят, что данные читаются под своим мьютексом.
 */

#include "line.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// Закрывает семафоры постов стадий [0, stages) и координатора (если открыт).
static void CloseThreads(Line *l, int stages, int coord) {
    for (int s = 0; s < stages; ++s) {
        for (int p = 0; p < l->cfg->stage[s].posts; ++p) {
            SimThreadDestroy(&l->stage[s].post[p].th);
        }
    }
    if (coord) {
        SimThreadDestroy(&l->coordTh);
    }
}

/// Готовит посты стадии s и открывает их семафоры. @return 0 или -1 (открытые закрыты)
static int InitPosts(Line *l, int s) {
    const StageSpec *spec = &l->cfg->stage[s];
    for (int p = 0; p < MAX_POSTS; ++p) {
        Post *post = &l->stage[s].post[p];
        post->line = l;
        post->stage = s;
        post->index = p;
        post->capacity = spec->capacity;
        for (int k = 0; k < MAX_CAPACITY; ++k) {
            post->seat[k] = (Seat){.car = -1, .left = 0, .duration = 0};
        }
        RngInit(&post->rng, l->cfg->seed, 100U + ((unsigned)s * MAX_POSTS) + (unsigned)p);
        if (p >= spec->posts) {
            continue;
        }
        char name[24];
        snprintf(name, sizeof name, "post %s.%d", spec->name, p + 1);
        if (SimThreadInit(&post->th, TH_POST, (s * MAX_POSTS) + p, name,
                          ((MAX_STAGES - s) * MAX_POSTS) + p) != 0) {
            for (int q = 0; q < p; ++q) {
                SimThreadDestroy(&l->stage[s].post[q].th);
            }
            return -1;
        }
    }
    return 0;
}

int LineInit(Line *l, const Config *cfg) {
    memset(l, 0, sizeof(*l));
    l->cfg = cfg;
    for (int s = 0; s < cfg->stages; ++s) {
        if (InitPosts(l, s) != 0) {
            CloseThreads(l, s, 0);
            return -1;
        }
    }
    if (SimThreadInit(&l->coordTh, TH_COORD, 0, "coordinator", 1000) != 0) {
        CloseThreads(l, cfg->stages, 0);
        return -1;
    }
    RngInit(&l->coordRng, cfg->seed, 1);
    for (int s = 0; s < cfg->stages; ++s) {
        Stage *st = &l->stage[s];
        st->index = s;
        MutexInit(&st->m, RANK_STAGE, cfg->stage[s].name);
        MutexLock(&st->m);
        st->queued = 0;
        st->queueReserved = 0;
        st->waiters = 0;
        MutexUnlock(&st->m);
    }
    MutexInit(&l->counters, RANK_LINE, "line counters");
    MutexInit(&l->registry, RANK_REGISTRY, "car registry");
    MutexLock(&l->counters);
    l->open = 1;
    l->nextTicket = 0;
    MutexUnlock(&l->counters);
    l->status = LINE_RUNNING;
    return 0;
}

void LineFree(Line *l) {
    CloseThreads(l, l->cfg->stages, 1);
    MutexLock(&l->registry);
    for (int i = 0; i < l->carCount; ++i) {
        SimThreadDestroy(&l->cars[i]->th);
        free(l->cars[i]);
    }
    free((void *)l->cars);
    l->cars = NULL;
    l->carCount = l->carCap = 0;
    MutexUnlock(&l->registry);
    for (int s = 0; s < l->cfg->stages; ++s) {
        MutexDestroy(&l->stage[s].m);
    }
    MutexDestroy(&l->counters);
    MutexDestroy(&l->registry);
}

Car *LineCar(Line *l, int id) {
    MutexLock(&l->registry);
    Car *car = id >= 0 && id < l->carCount ? l->cars[id] : NULL;
    MutexUnlock(&l->registry);
    return car;
}

int CarNextStage(const Car *car, const Config *cfg) {
    const ProgramSpec *p = &cfg->program[car->program];
    return car->step + 1 < p->len ? p->route[car->step + 1] : -1;
}

int CarRemainingWork(const Car *car, const Config *cfg) {
    const ProgramSpec *p = &cfg->program[car->program];
    int work = 0;
    for (int k = car->step; k < p->len; ++k) {
        const StageSpec *s = &cfg->stage[p->route[k]];
        work += (s->tmin + s->tmax) / 2;
    }
    return work;
}

/// Куда хочет уехать помытая машина с места seat (left == 0), или -1.
static int SeatWants(Line *l, const Seat *seat) {
    if (seat->car < 0 || seat->left != 0) {
        return -1;
    }
    const Car *car = LineCar(l, seat->car);
    return car ? CarNextStage(car, l->cfg) : -1;
}

/// Отмечает в wanted стадии, куда хотят уехать помытые машины стадии st.
static void MarkWanted(Line *l, Stage *st, int wanted[MAX_STAGES]) REQUIRES(st->m) {
    const StageSpec *spec = &l->cfg->stage[st->index];
    for (int p = 0; p < spec->posts; ++p) {
        for (int k = 0; k < spec->capacity; ++k) {
            int next = SeatWants(l, &st->post[p].seat[k]);
            if (next >= 0) {
                wanted[next] = 1;
            }
        }
    }
}

/// Может ли что-то сдвинуться на стадии st (wanted - куда хотят помытые машины).
static int StageMayProgress(const Line *l, Stage *st, const int wanted[MAX_STAGES])
    REQUIRES(st->m) {
    const StageSpec *spec = &l->cfg->stage[st->index];
    if (st->queueReserved > 0) {
        return 1;  // бронь очереди: машина уже едет (в покое не бывает)
    }
    for (int p = 0; p < spec->posts; ++p) {
        const Post *post = &st->post[p];
        int occupied = 0, busy = 0;
        for (int k = 0; k < spec->capacity; ++k) {
            const Seat *seat = &post->seat[k];
            occupied = occupied || seat->car >= 0;
            busy = busy || seat->left != 0;  // идёт операция или машина едет на забронированное
        }
        if (post->repair == 0 && busy) {
            return 1;
        }
        // ремонт пустого поста, которого никто не ждёт, не считается (как в ИЗ1)
        if (post->repair > 0 && (occupied || st->queued > 0 || wanted[st->index])) {
            return 1;
        }
    }
    return 0;
}

int LineProgressPossible(Line *l) {
    const Config *cfg = l->cfg;
    int wanted[MAX_STAGES] = {0};  // куда хотят уехать уже помытые машины
    for (int s = 0; s < cfg->stages; ++s) {
        Stage *st = &l->stage[s];
        MutexLock(&st->m);
        MarkWanted(l, st, wanted);
        MutexUnlock(&st->m);
    }
    for (int s = 0; s < cfg->stages; ++s) {
        Stage *st = &l->stage[s];
        MutexLock(&st->m);
        int may = StageMayProgress(l, st, wanted);
        MutexUnlock(&st->m);
        if (may) {
            return 1;
        }
    }
    return 0;
}

/// Есть ли на стадии st место в очереди или свободное место на посту (даже сломанном).
static int StageHasRoom(const Line *l, Stage *st) REQUIRES(st->m) {
    const StageSpec *spec = &l->cfg->stage[st->index];
    if (st->queued + st->queueReserved < spec->queue) {
        return 1;
    }
    for (int p = 0; p < spec->posts; ++p) {
        for (int k = 0; k < spec->capacity; ++k) {
            if (st->post[p].seat[k].car < 0) {
                return 1;
            }
        }
    }
    return 0;
}

int LineEntranceMayAccept(Line *l) {
    const Config *cfg = l->cfg;
    for (int i = 0; i < cfg->programs; ++i) {
        Stage *st = &l->stage[cfg->program[i].route[0]];
        MutexLock(&st->m);
        int room = StageHasRoom(l, st);
        MutexUnlock(&st->m);
        if (room) {
            return 1;
        }
    }
    return 0;
}

/// Стадия без постов, в которую упёрлись машины стадии st (очередь или помытые на постах), или -1.
static int StageStallCause(Line *l, Stage *st) REQUIRES(st->m) {
    const Config *cfg = l->cfg;
    const StageSpec *spec = &cfg->stage[st->index];
    if (spec->posts == 0 && (st->queued > 0 || st->waiters > 0)) {
        return st->index;
    }
    for (int p = 0; p < spec->posts; ++p) {
        for (int k = 0; k < spec->capacity; ++k) {
            int next = SeatWants(l, &st->post[p].seat[k]);
            if (next >= 0 && cfg->stage[next].posts == 0) {
                return next;
            }
        }
    }
    return -1;
}

int LineStallCause(Line *l) {
    for (int s = 0; s < l->cfg->stages; ++s) {
        Stage *st = &l->stage[s];
        MutexLock(&st->m);
        int cause = StageStallCause(l, st);
        MutexUnlock(&st->m);
        if (cause >= 0) {
            return cause;
        }
    }
    return -1;
}
