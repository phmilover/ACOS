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

/// Уничтожает условные переменные и потоки первых @p posts постов стадии s.
static void DestroyPosts(Line *l, int s, int posts) {
    for (int p = 0; p < posts; ++p) {
        CondDestroy(&l->stage[s].post[p].done);
        SimThreadDestroy(&l->stage[s].post[p].th);
    }
}

/// Уничтожает всё, что InitStage создала для стадий [0, stages).
static void DestroyStages(Line *l, int stages) {
    for (int s = 0; s < stages; ++s) {
        DestroyPosts(l, s, l->cfg->stage[s].posts);
        CondDestroy(&l->stage[s].seatFree);
        CondDestroy(&l->stage[s].room);
    }
}

/// Готовит пост p стадии s: поля, ГПСЧ; у настоящего поста - условная переменная и поток.
/// @return 0 или -1 (созданное для этого поста уничтожено)
static int InitPost(Line *l, int s, int p) {
    const StageSpec *spec = &l->cfg->stage[s];
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
        return 0;
    }
    if (CondInit(&post->done) != 0) {
        return -1;
    }
    char name[24];
    snprintf(name, sizeof name, "post %s.%d", spec->name, p + 1);
    if (SimThreadInit(&post->th, TH_POST, (s * MAX_POSTS) + p, name,
                      ((MAX_STAGES - s) * MAX_POSTS) + p) != 0) {
        CondDestroy(&post->done);
        return -1;
    }
    return 0;
}

/// Условные переменные и посты стадии s. @return 0 или -1 (созданное для стадии уничтожено)
static int InitStage(Line *l, int s) {
    Stage *st = &l->stage[s];
    if (CondInit(&st->seatFree) != 0) {
        return -1;
    }
    if (CondInit(&st->room) != 0) {
        CondDestroy(&st->seatFree);
        return -1;
    }
    for (int p = 0; p < MAX_POSTS; ++p) {
        if (InitPost(l, s, p) != 0) {
            DestroyPosts(l, s, p);
            CondDestroy(&st->seatFree);
            CondDestroy(&st->room);
            return -1;
        }
    }
    return 0;
}

/// Координатор, реестр и въезд. @return 0 или -1 (созданное здесь уничтожено)
static int InitShared(Line *l) {
    if (SimThreadInit(&l->coordTh, TH_COORD, 0, "coordinator", 1000) != 0) {
        return -1;
    }
    if (RwLockInit(&l->registry, RANK_REGISTRY, "car registry") != 0) {
        SimThreadDestroy(&l->coordTh);
        return -1;
    }
    if (CondInit(&l->decided) != 0) {
        RwLockDestroy(&l->registry);
        SimThreadDestroy(&l->coordTh);
        return -1;
    }
    MutexInit(&l->entrance, RANK_ENTRANCE, "entrance");
    return 0;
}

int LineInit(Line *l, const Config *cfg) {
    memset(l, 0, sizeof(*l));
    l->cfg = cfg;
    for (int s = 0; s < cfg->stages; ++s) {
        if (InitStage(l, s) != 0) {
            DestroyStages(l, s);
            return -1;
        }
    }
    if (InitShared(l) != 0) {
        DestroyStages(l, cfg->stages);
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
    RwLockWrite(&l->registry);
    l->cars = NULL;
    l->carCount = l->carCap = 0;
    RwLockUnlock(&l->registry);
    atomic_store(&l->open, 1);
    atomic_store(&l->nextTicket, 0);
    l->status = LINE_RUNNING;
    l->sampledAt = -1;
    return 0;
}

void LineFree(Line *l) {
    DestroyStages(l, l->cfg->stages);
    SimThreadDestroy(&l->coordTh);
    RwLockWrite(&l->registry);
    for (int i = 0; i < l->carCount; ++i) {
        SimThreadDestroy(&l->cars[i]->th);
        free(l->cars[i]);
    }
    free((void *)l->cars);
    l->cars = NULL;
    l->carCount = l->carCap = 0;
    RwLockUnlock(&l->registry);
    for (int s = 0; s < l->cfg->stages; ++s) {
        MutexDestroy(&l->stage[s].m);
    }
    CondDestroy(&l->decided);
    MutexDestroy(&l->entrance);
    RwLockDestroy(&l->registry);
}

Car *LineCar(Line *l, int id) {
    RwLockRead(&l->registry);
    Car *car = id >= 0 && id < l->carCount ? l->cars[id] : NULL;
    RwLockUnlock(&l->registry);
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

int LineExitCode(LineStatus status, long audit, long lockdep, long soft) {
    if (audit > 0 || lockdep > 0 || soft > 0) {
        return 4;
    }
    switch (status) {
    case LINE_FINISHED: return 0;
    case LINE_STALLED: return 2;
    case LINE_NO_RESOURCES: return 5;
    default: return 3;
    }
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
