/**
 * @file car.c
 * @brief Поток машины.
 *
 * Машина сама переезжает со стадии на стадию: бронирует место на следующей стадии, держа
 * текущее, и только потом освобождает текущее и занимает забронированное (StageMove).
 * Мьютексы стадий не вкладываются: StageVacate под мьютексом текущей, затем StageCommit под
 * мьютексом следующей (события LEAVE, MOVE, ENTER/QUEUE как в ИЗ1). Все ожидания - в мониторе
 * стадии (stage.h): машина ждёт своего условия в цикле while и сама проверяет предикат.
 *
 * Поля машины (место, бронь) читаются только под мьютексом той стадии, где она стоит: из
 * очереди на пост машина садится сама (StageAwaitSeat), но причину ожидания за неё меняют
 * посты (поломка, ремонт).
 */

#include "car.h"

#include <stdio.h>
#include <stdlib.h>

#include "events.h"
#include "memory.h"
#include "soft_assert.h"
#include "stage.h"

/// peakAlive = max(peakAlive, alive) без мьютекса.
static void RaisePeak(Line *l, int alive) {
    int peak = atomic_load(&l->peakAlive);
    while (alive > peak && !atomic_compare_exchange_weak(&l->peakAlive, &peak, alive)) {
    }
}

Car *CarCreate(Line *l, int now) {
    Car *car = calloc(1, sizeof *car);
    if (car == NULL) {
        return NULL;
    }
    RwLockWrite(&l->registry);
    car->id = l->carCount;
    char name[24];
    snprintf(name, sizeof name, "car #%d", car->id + 1);
    // order у машины не важен: по времени она не спит, а разрешённые идут в порядке FIFO
    if (SimThreadInit(&car->th, TH_CAR, car->id, name, 2000 + car->id) != 0) {
        RwLockUnlock(&l->registry);
        free(car);
        return NULL;
    }
    if (l->carCount == l->carCap) {
        l->carCap = l->carCap ? l->carCap * 2 : 64;
        l->cars = (Car **)CheckedRealloc((void *)l->cars, (size_t)l->carCap * sizeof *l->cars);
    }
    l->cars[l->carCount++] = car;
    RwLockUnlock(&l->registry);
    RaisePeak(l, atomic_fetch_add(&l->alive, 1) + 1);

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

/// Последняя машина дня решила вопрос с въездом: будит координатора, он закроет въезд.
static void WakeCoordinator(Line *l) {
    MutexLock(&l->entrance);
    SimGrant(&l->coordTh, &l->decided);
    CondBroadcast(&l->decided);
    MutexUnlock(&l->entrance);
}

/// Въезд на первую стадию. @return 1 - машина на линии, 0 - отказ
static int Enter(Line *l, Car *car, int first) {
    Stage *st = &l->stage[first];
    MutexLock(&st->m);
    int accepted = StageEnter(l, st, car) != ADMIT_FULL;
    if (accepted) {
        atomic_fetch_add(&l->inside, 1);
    } else {
        car->place = CAR_REJECTED;
        car->finished = car->arrived;
        EV(EV_REJECT, car->id, first, -1, car->program, st->queued + st->queueReserved);
        atomic_fetch_add(&l->rejected, 1);
    }
    MutexUnlock(&st->m);
    // после inside: координатор, увидев всех решёнными, считает принятых для EV_CLOSE
    int decided = atomic_fetch_add(&l->arrivedCount, 1) + 1;
    if (l->cfg->cars > 0 && decided == l->cfg->cars) {
        WakeCoordinator(l);
    }
    return accepted;
}

/// Последняя стадия: уезжает с мойки.
static void Finish(Line *l, Car *car, Stage *st) {
    MutexLock(&st->m);
    StageVacate(l, st, car);
    car->place = CAR_DONE;
    car->stage = -1;
    car->finished = SimNow();
    EventsEmit((Event){.type = EV_DONE,
                       .car = car->id,
                       .stage = st->index,
                       .post = -1,
                       .program = car->program,
                       .arg = car->finished - car->arrived,
                       .arg2 = car->service});
    MutexUnlock(&st->m);
    atomic_fetch_add(&l->served, 1);
    atomic_fetch_sub(&l->inside, 1);
}

/// Ждёт места на посту (если машина в очереди) и конца операции на стадии st.
/// @return SIM_OK и пост машины в *post или SIM_STOP
static SimStatus WashOn(Line *l, Car *car, Stage *st, int *post) {
    MutexLock(&st->m);
    SimStatus rc = car->place == CAR_QUEUE ? StageAwaitSeat(l, st, car) : SIM_OK;
    if (rc == SIM_OK) {
        rc = StageAwaitOperation(l, st, car);
    }
    *post = car->post;
    MutexUnlock(&st->m);
    return rc;
}

/// Бронь места на стадии next, стоя на посту post стадии cur. @return SIM_OK или SIM_STOP
static SimStatus Reserve(Line *l, Car *car, int cur, int post, int next) {
    Stage *ns = &l->stage[next];
    MutexLock(&ns->m);
    SimStatus rc = StageReserve(l, ns, car, cur, post);
    MutexUnlock(&ns->m);
    return rc;
}

/// Маршрут машины, принятой на въезде.
static void Ride(Line *l, Car *car, int cur) {
    for (;;) {
        int post = -1;
        if (WashOn(l, car, &l->stage[cur], &post) == SIM_STOP) {
            return;
        }
        int next = CarNextStage(car, l->cfg);
        if (next < 0) {
            Finish(l, car, &l->stage[cur]);
            return;
        }
        if (Reserve(l, car, cur, post, next) == SIM_STOP) {
            return;
        }
        // бронь уже есть, место за машиной никто не займёт: мьютексы стадий не вкладываются
        StageMove(l, &l->stage[cur], &l->stage[next], car);
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
