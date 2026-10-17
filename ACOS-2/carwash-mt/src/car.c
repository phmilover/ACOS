/**
 * @file car.c
 * @brief Поток машины.
 *
 * Машина сама переезжает со стадии на стадию по «эстафете» (stage.h): бронирует место на
 * следующей стадии, держа текущее, и только потом освобождает текущее и занимает
 * забронированное. Мьютексы стадий не вкладываются: StageVacate под мьютексом текущей,
 * затем StageCommit под мьютексом следующей (события LEAVE, MOVE, ENTER/QUEUE как в ИЗ1).
 *
 * На какой стадии машина, знает только её поток (переменная cur): поля car->stage и
 * car->place за машину может переписать другой поток (StageDispatch сажает её из очереди на
 * пост), поэтому они читаются только под мьютексом стадии.
 */

#include "car.h"

#include <stdio.h>
#include <stdlib.h>

#include "events.h"
#include "memory.h"
#include "soft_assert.h"
#include "stage.h"

Car *CarCreate(Line *l, int now) {
    Car *car = calloc(1, sizeof *car);
    if (car == NULL) {
        return NULL;
    }
    MutexLock(&l->registry);
    car->id = l->carCount;
    char name[24];
    snprintf(name, sizeof name, "car #%d", car->id + 1);
    // order у машины не важен: по времени она не спит, а разбуженные идут в порядке FIFO
    if (SimThreadInit(&car->th, TH_CAR, car->id, name, 2000 + car->id) != 0) {
        MutexUnlock(&l->registry);
        free(car);
        return NULL;
    }
    if (l->carCount == l->carCap) {
        l->carCap = l->carCap ? l->carCap * 2 : 64;
        l->cars = (Car **)CheckedRealloc((void *)l->cars, (size_t)l->carCap * sizeof *l->cars);
    }
    l->cars[l->carCount++] = car;
    if (++l->alive > l->peakAlive) {
        l->peakAlive = l->alive;
    }
    MutexUnlock(&l->registry);

    car->line = l;
    RngInit(&car->rng, l->cfg->seed, 1000U + (unsigned)car->id);
    car->program = -1;
    car->place = CAR_ARRIVING;
    car->stage = car->post = car->seat = -1;
    car->res = RES_NONE;
    car->resStage = car->resPost = car->resSeat = -1;
    car->ticket = -1;
    car->arrived = now;
    car->finished = -1;
    atomic_init(&car->over, 0);
    return car;
}

/// Программа по весам: число от 1 до суммы весов - в чей отрезок попало (как в ИЗ1).
static int ChooseProgram(Car *car, const Config *cfg) {
    int total = 0;
    for (int i = 0; i < cfg->programs; ++i) {
        total += cfg->program[i].weight;
    }
    SOFT_ASSERT(total > 0, "programs have no weight");
    int roll = RngRange(&car->rng, 1, total);
    int p = 0;
    while (p + 1 < cfg->programs && roll > cfg->program[p].weight) {
        roll -= cfg->program[p++].weight;
    }
    return p;
}

/// Въезд на первую стадию. @return 1 - машина на линии, 0 - отказ
static int Enter(Line *l, Car *car, int first) {
    Stage *st = &l->stage[first];
    MutexLock(&st->m);
    int accepted = StageAdmit(l, st, car, 0) != ADMIT_FULL;
    if (!accepted) {
        car->place = CAR_REJECTED;
        car->finished = car->arrived;
        EV(EV_REJECT, car->id, first, -1, car->program, st->queued + st->queueReserved);
    }
    MutexLock(&l->counters);
    if (accepted) {
        l->inside++;
    } else {
        l->rejected++;
    }
    int decided = ++l->arrivedCount;  // машин, для которых въезд уже решён
    MutexUnlock(&l->counters);
    if (accepted) {
        StageCommit(l, st, car, -1);
    }
    MutexUnlock(&st->m);
    if (l->cfg->cars > 0 && decided == l->cfg->cars) {
        SimWake(&l->coordTh);  // последняя машина дня: координатор закроет въезд
    }
    return accepted;
}

/// Ждёт конца операции на стадии cur. @return SIM_OK и пост машины в *post или SIM_STOP
static SimStatus WaitOperation(Line *l, Car *car, int cur, int *post) {
    Stage *st = &l->stage[cur];
    for (;;) {
        MutexLock(&st->m);
        int onPost = car->place == CAR_POST;
        int done = onPost && st->post[car->post].seat[car->seat].left == 0;
        WaitReason why = {.kind = onPost ? WAIT_OPERATION : WAIT_QUEUE,
                          .stage = cur,
                          .post = onPost ? car->post : -1};
        *post = car->post;
        MutexUnlock(&st->m);
        if (done) {
            return SIM_OK;
        }
        // будит пост в конце операции; если машину сажают из очереди, пока она не уснула,
        // StageDispatch сообщает часам новую причину (SimSetReason) - она переживёт SimPark
        if (SimPark(&car->th, why) == SIM_STOP) {
            return SIM_STOP;
        }
    }
}

/// Последняя стадия: уезжает с мойки.
static void Finish(Line *l, Car *car, int cur) {
    Stage *st = &l->stage[cur];
    MutexLock(&st->m);
    StageVacate(l, st, car);
    car->place = CAR_DONE;
    car->stage = -1;
    car->finished = SimNow();
    EventsEmit((Event){.type = EV_DONE,
                       .car = car->id,
                       .stage = cur,
                       .post = -1,
                       .program = car->program,
                       .arg = car->finished - car->arrived,
                       .arg2 = car->service});
    MutexUnlock(&st->m);
    MutexLock(&l->counters);
    l->served++;
    l->inside--;
    MutexUnlock(&l->counters);
}

/// Бронирует место на стадии next, стоя на посту post стадии cur. @return SIM_OK или SIM_STOP
static SimStatus Reserve(Line *l, Car *car, int cur, int post, int next) {
    Stage *ns = &l->stage[next];
    int blocked = 0;
    for (;;) {
        MutexLock(&ns->m);
        Admit a = StageAdmit(l, ns, car, 1);  // повторный вызов безопасен (stage.h)
        if (a == ADMIT_FULL && !blocked) {
            blocked = 1;
            EV(EV_BLOCKED, car->id, cur, post, car->program, next);
        }
        MutexUnlock(&ns->m);
        if (a != ADMIT_FULL) {
            return SIM_OK;
        }
        // бронь выдаст и разбудит тот, кто освободит место на стадии next
        WaitReason why = {.kind = WAIT_ROOM, .stage = next, .post = -1};
        if (SimPark(&car->th, why) == SIM_STOP) {
            return SIM_STOP;
        }
    }
}

/// Маршрут машины, принятой на въезде.
static void Ride(Line *l, Car *car, int cur) {
    for (;;) {
        int post = -1;
        if (WaitOperation(l, car, cur, &post) == SIM_STOP) {
            return;
        }
        int next = CarNextStage(car, l->cfg);
        if (next < 0) {
            Finish(l, car, cur);
            return;
        }
        if (Reserve(l, car, cur, post, next) == SIM_STOP) {
            return;
        }
        // мьютексы стадий не вкладываются: бронь уже есть, место за машиной никто не займёт
        Stage *st = &l->stage[cur], *ns = &l->stage[next];
        MutexLock(&st->m);
        StageVacate(l, st, car);
        MutexUnlock(&st->m);
        MutexLock(&ns->m);
        StageCommit(l, ns, car, cur);
        MutexUnlock(&ns->m);
        cur = next;
    }
}

void *CarMain(void *arg) {
    Car *car = arg;
    Line *l = car->line;
    if (SimBegin(&car->th) == SIM_OK) {
        EventsSetSource(&car->th);
        car->program = ChooseProgram(car, l->cfg);
        EV(EV_ARRIVE, car->id, -1, -1, car->program, 0);
        EV(EV_PROGRAM, car->id, -1, -1, car->program, car->program);
        int first = l->cfg->program[car->program].route[0];
        if (Enter(l, car, first)) {
            Ride(l, car, first);
        }
        if (car->finished >= 0) {
            atomic_store(&car->over, 1);  // уехала или отказ: координатор соберёт поток
        }
    }
    SimExit(&car->th);
    return NULL;
}
