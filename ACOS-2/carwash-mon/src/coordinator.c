/**
 * @file coordinator.c
 * @brief Поток координатора: прибытия, закрытие въезда, сбор потоков машин.
 *
 * Порядок дел за минуту как в LineTick ИЗ1: сначала проверка конца дня, затем прибытие.
 * В -D координатор будится последним в минуте (order 1000), после всех постов.
 */

#include "coordinator.h"

#include <pthread.h>

#include "car.h"
#include "events.h"
#include "line.h"
#include "thread.h"

/// Закрывает въезд: EV_CLOSE с числом принятых машин внутри. Если въезд уже закрыт, второго
/// EV_CLOSE нет.
static void Close(Line *l) {
    if (atomic_exchange(&l->open, 0)) {
        EV(EV_CLOSE, -1, -1, -1, -1, atomic_load(&l->inside));
    }
}

/// Ждёт, пока все cfg->cars машин решат вопрос с въездом (принята или отказ): так EV_CLOSE
/// идёт после событий последней машины и считает её среди принятых, как в ИЗ1.
/// @return SIM_OK или SIM_STOP
static SimStatus WaitLastArrival(Line *l) {
    SimStatus rc = SIM_OK;
    MutexLock(&l->entrance);
    // будит последняя машина (Enter в car.c) под тем же мьютексом: SimGrant, CondBroadcast
    while (rc == SIM_OK && atomic_load(&l->arrivedCount) < l->cfg->cars) {
        rc = SimWaitOn(&l->coordTh, &l->decided, &l->entrance,
                       (WaitReason){.kind = WAIT_NONE, .stage = -1, .post = -1});
    }
    MutexUnlock(&l->entrance);
    return rc;
}

/// Создаёт машину и её поток. @return 0 или -1 (нет памяти, условной переменной или потока)
static int SpawnCar(Line *l, int now) {
    Car *car = CarCreate(l, now);
    if (car == NULL) {
        return -1;
    }
    SimSpawned(&car->th);  // до ThreadCreate: часы не должны увидеть ложный покой
    if (ThreadCreate(&car->tid, CarMain, car, CAR_STACK_SIZE) != 0) {
        SimExit(&car->th);  // поток не появился: снимаем его из очереди запуска за него
        car->joined = 1;
        return -1;
    }
    return 0;
}

/// Собирает потоки машин, которые уехали или получили отказ.
/// @param from первая несобранная машина (все до неё уже собраны)
/// @return новая первая несобранная
static int Reap(Line *l, int from) {
    int first = -1, i = from;
    Car *car;
    for (; (car = LineCar(l, i)) != NULL; ++i) {  // registry не держим во время join
        if (!car->joined && atomic_load(&car->over)) {
            pthread_join(car->tid, NULL);
            car->joined = 1;
            // машина больше не поток модели: снимаем её с учёта часов. Иначе часы каждую
            // минуту обходили бы все когда-либо приезжавшие машины, и в режиме -u прогон
            // замедлялся бы всё сильнее. Повторный вызов в LineFree безопасен
            SimThreadDestroy(&car->th);
            atomic_fetch_sub(&l->alive, 1);
        }
        if (!car->joined && first < 0) {
            first = i;
        }
    }
    return first < 0 ? i : first;
}

/// @return сколько машин создано
static int CarsCreated(Line *l) {
    RwLockRead(&l->registry);
    int count = l->carCount;
    RwLockUnlock(&l->registry);
    return count;
}

void *CoordinatorMain(void *arg) {
    Line *l = arg;
    const Config *cfg = l->cfg;
    BarrierWait(&l->start);
    if (SimBegin(&l->coordTh) == SIM_OK) {
        EventsSetSource(&l->coordTh);
        int nextArrival = 0;  // первая машина уже ждёт открытия
        int reaped = 0;
        for (;;) {
            int now = SimNow();
            if (!atomic_load(&l->open)) {
                break;  // въезд уже закрыт: новых машин не будет
            }
            if (cfg->day > 0 && now >= cfg->day) {
                Close(l);
                break;
            }
            // сначала собрать уехавших, потом впускать новую: тогда живых потоков машин не
            // больше, чем мест на линии, плюс одна новая (все решения прошлых минут уже приняты)
            reaped = Reap(l, reaped);
            if (now >= nextArrival) {
                if (SpawnCar(l, now) != 0) {
                    atomic_fetch_or(&l->requests, REQ_NO_RESOURCES);
                    break;
                }
                nextArrival = now + RngRange(&l->coordRng, cfg->arrMin, cfg->arrMax);
                if (cfg->cars > 0 && CarsCreated(l) >= cfg->cars) {
                    if (WaitLastArrival(l) == SIM_OK) {
                        Close(l);
                    }
                    break;
                }
            }
            int wake = nextArrival;
            if (cfg->day > 0 && cfg->day < wake) {
                wake = cfg->day;
            }
            if (SimSleep(&l->coordTh, wake > now ? wake - now : 1) == SIM_STOP) {
                break;
            }
        }
    }
    SimExit(&l->coordTh);
    return NULL;
}
