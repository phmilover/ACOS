/**
 * @file stage.c
 * @brief «Передача эстафеты» (passing the baton, Г. Эндрюс) на одной стадии.
 *
 * Всё под мьютексом стадии. Бронь места на посту - это место с car = машина и left < 0:
 * оно уже не свободно, а post->reserved считает такие места. Бронь места в очереди считает
 * queueReserved, и она входит в проверку вместимости. Поэтому пока машина переезжает, её
 * будущее место никто не займёт (I5), а сама она числится только на старом месте (I2).
 */

#include "stage.h"

#include "events.h"
#include "soft_assert.h"

/// Сколько машин можно держать в очереди стадии. С SAB_OVERFLOW - специально на одну больше.
static int QueueLimit(const Line *l, int s) {
    int cap = l->cfg->stage[s].queue + (l->cfg->sabotage == SAB_OVERFLOW ? 1 : 0);
    return cap < MAX_QUEUE ? cap : MAX_QUEUE;
}

static int QueueHasRoom(const Line *l, Stage *st) REQUIRES(st->m) {
    return st->queued + st->queueReserved < QueueLimit(l, st->index);
}

/// Очередь пуста, и никто в неё не едет.
static int QueueEmpty(Stage *st) REQUIRES(st->m) {
    return st->queued == 0 && st->queueReserved == 0;
}

/// Первое свободное место на исправном посту. @return 1 и пост/место в *p, *k, иначе 0
static int FreeSeat(const Line *l, Stage *st, int *p, int *k) REQUIRES(st->m) {
    const StageSpec *spec = &l->cfg->stage[st->index];
    for (int i = 0; i < spec->posts; ++i) {
        if (st->post[i].repair > 0) {
            continue;  // неисправный пост новых машин не берёт (I4)
        }
        for (int j = 0; j < spec->capacity; ++j) {
            if (st->post[i].seat[j].car < 0) {
                *p = i;
                *k = j;
                return 1;
            }
        }
    }
    return 0;
}

int StageScore(const Line *l, const Car *car) {
    switch (l->cfg->rule) {
    case PRIO_PROGRAM: return l->cfg->program[car->program].priority;
    case PRIO_SHORTEST: return -CarRemainingWork(car, l->cfg);
    default: return 0;  // fifo: оценки равны, решает порядок
    }
}

/// Бронь места k поста p за машиной.
static void ReserveSeat(Stage *st, Car *car, int p, int k) REQUIRES(st->m) {
    st->post[p].seat[k] = (Seat){.car = car->id, .left = -1, .duration = 0};
    st->post[p].reserved++;
    car->res = RES_SEAT;
    car->resStage = st->index;
    car->resPost = p;
    car->resSeat = k;
}

static void ReserveQueue(Stage *st, Car *car) REQUIRES(st->m) {
    st->queueReserved++;
    car->res = RES_QUEUE;
    car->resStage = st->index;
    car->resPost = car->resSeat = -1;
}

/// Ставит машину в список ожидающих стадии. @return 0 или -1, если список полон
static int AddWaiter(Line *l, Stage *st, Car *car) REQUIRES(st->m) {
    if (!SOFT_ASSERT(st->waiters < MAX_WAITERS, "waiting list is full")) {
        return -1;
    }
    MutexLock(&l->counters);
    car->ticket = ++l->nextTicket;
    MutexUnlock(&l->counters);
    st->waiting[st->waiters++] = car;
    return 0;
}

/// Ждёт ли машина в списке стадии.
static int IsWaiting(Stage *st, const Car *car) REQUIRES(st->m) {
    for (int i = 0; i < st->waiters; ++i) {
        if (st->waiting[i] == car) {
            return 1;
        }
    }
    return 0;
}

Admit StageAdmit(Line *l, Stage *st, Car *car, int mayWait) {
    if (car->res != RES_NONE && car->resStage == st->index) {
        return car->res == RES_SEAT ? ADMIT_SEAT : ADMIT_QUEUE;  // бронь уже выдана
    }
    if (!SOFT_ASSERT(car->res == RES_NONE, "car already holds a reservation elsewhere")) {
        return ADMIT_FULL;
    }
    if (car->ticket >= 0 && IsWaiting(st, car)) {
        return ADMIT_FULL;  // уже ждёт: второй раз в список не встаёт
    }
    int p = -1, k = -1;
    if (st->waiters == 0 && QueueEmpty(st) && FreeSeat(l, st, &p, &k)) {
        ReserveSeat(st, car, p, k);
        return ADMIT_SEAT;
    }
    if (st->waiters == 0 && QueueHasRoom(l, st)) {
        ReserveQueue(st, car);
        return ADMIT_QUEUE;
    }
    if (mayWait) {
        AddWaiter(l, st, car);
    }
    return ADMIT_FULL;
}

/// Индекс стадии s в маршруте машины начиная с текущего шага (или текущий шаг, если не нашли).
static int RouteIndex(const Line *l, const Car *car, int s) {
    const ProgramSpec *prog = &l->cfg->program[car->program];
    for (int i = car->step; i < prog->len; ++i) {
        if (prog->route[i] == s) {
            return i;
        }
    }
    SOFT_ASSERT(0, "stage is not on the car's route");
    return car->step;
}

/// Машина садится на место k поста p и начинает операцию: EV_ENTER, EV_OP_START.
static void StartOperation(Line *l, Stage *st, Car *car, int p, int k) REQUIRES(st->m) {
    const StageSpec *spec = &l->cfg->stage[st->index];
    Post *post = &st->post[p];
    SOFT_ASSERT(post->repair == 0, "operation starts on a broken post");
    int duration = RngRange(&post->rng, spec->tmin, spec->tmax);
    post->seat[k] =
        (Seat){.car = car->id, .left = duration, .duration = duration, .start = SimNow()};
    car->place = CAR_POST;
    car->stage = st->index;
    car->post = p;
    car->seat = k;
    car->service += duration;
    EV(EV_ENTER, car->id, st->index, p, car->program, 0);
    EV(EV_OP_START, car->id, st->index, p, car->program, duration);
}

void StageCommit(Line *l, Stage *st, Car *car, int fromStage) {
    int s = st->index;
    if (!SOFT_ASSERT(car->res != RES_NONE && car->resStage == s, "commit without a reservation")) {
        return;
    }
    car->step = RouteIndex(l, car, s);
    if (fromStage >= 0) {
        EV(EV_MOVE, car->id, s, -1, car->program, fromStage);
    }
    if (car->res == RES_SEAT) {
        int p = car->resPost, k = car->resSeat;
        st->post[p].reserved--;
        car->res = RES_NONE;
        StartOperation(l, st, car, p, k);
    } else {
        st->queueReserved--;
        car->res = RES_NONE;
        // с SAB_OVERFLOW StageAdmit пропускает лишнюю машину, и это ловится здесь (как в ИЗ1)
        SOFT_ASSERT(st->queued < l->cfg->stage[s].queue, "no room in the queue");
        if (SOFT_ASSERT(st->queued < MAX_QUEUE, "queue array is full")) {
            st->queue[st->queued++] = car->id;
        }
        car->place = CAR_QUEUE;
        car->stage = s;
        car->post = car->seat = -1;
        EV(EV_QUEUE, car->id, s, -1, car->program, st->queued);
    }
    car->resStage = car->resPost = car->resSeat = -1;
    StageDispatch(l, st);
}

void StageVacate(Line *l, Stage *st, Car *car) {
    const StageSpec *spec = &l->cfg->stage[st->index];
    // ищем место по постам стадии, а не по полям машины: StageCommit на следующей стадии мог
    // уже переписать car->stage и car->post
    for (int p = 0; p < spec->posts; ++p) {
        for (int k = 0; k < spec->capacity; ++k) {
            Seat *seat = &st->post[p].seat[k];
            if (seat->car != car->id || seat->left < 0) {
                continue;
            }
            SOFT_ASSERT(seat->left == 0, "car leaves before its operation is over");
            *seat = (Seat){.car = -1, .left = 0, .duration = 0};
            if (car->place == CAR_POST && car->stage == st->index) {
                car->post = car->seat = -1;  // машина в пути; место назначат Commit или конец
            }
            EV(EV_LEAVE, car->id, st->index, p, car->program, 0);
            StageDispatch(l, st);
            return;
        }
    }
    SOFT_ASSERT(0, "car is not on a post of this stage");
}

/// Лучшая машина в очереди: больше оценка, при равной - кто раньше встал. @return индекс
static int BestQueued(Line *l, Stage *st, Car **best) REQUIRES(st->m) {
    int bi = -1, bscore = 0;
    *best = NULL;
    for (int i = 0; i < st->queued; ++i) {
        Car *car = LineCar(l, st->queue[i]);
        if (!SOFT_ASSERT(car != NULL, "unknown car in the queue")) {
            continue;
        }
        int score = StageScore(l, car);
        if (bi < 0 || score > bscore) {
            bi = i;
            bscore = score;
            *best = car;
        }
    }
    return bi;
}

/// Кто из ожидающих первый: меньше ticket, затем выше оценка, затем меньший номер.
static int WaiterBefore(const Line *l, const Car *a, const Car *b) {
    if (a->ticket != b->ticket) {
        return a->ticket < b->ticket;
    }
    int sa = StageScore(l, a), sb = StageScore(l, b);
    if (sa != sb) {
        return sa > sb;
    }
    return a->id < b->id;
}

static int BestWaiter(const Line *l, Stage *st) REQUIRES(st->m) {
    int best = 0;
    for (int i = 1; i < st->waiters; ++i) {
        if (WaiterBefore(l, st->waiting[i], st->waiting[best])) {
            best = i;
        }
    }
    return best;
}

/// (1) машины из очереди - на свободные места исправных постов.
static void SeatQueued(Line *l, Stage *st) REQUIRES(st->m) {
    int p = -1, k = -1;
    while (st->queued > 0 && FreeSeat(l, st, &p, &k)) {
        Car *car = NULL;
        int i = BestQueued(l, st, &car);
        if (i < 0) {
            break;
        }
        for (int j = i; j + 1 < st->queued; ++j) {
            st->queue[j] = st->queue[j + 1];
        }
        st->queued--;
        StartOperation(l, st, car, p, k);
        // машина спит в очереди; теперь она ждёт конца операции, будит её пост
        SimSetReason(&car->th, (WaitReason){.kind = WAIT_OPERATION, .stage = st->index, .post = p});
    }
}

/// (2) ожидающим снаружи - бронь места на посту (если очередь пуста) или в очереди.
static void GrantWaiters(Line *l, Stage *st) REQUIRES(st->m) {
    while (st->waiters > 0) {
        int i = BestWaiter(l, st);
        Car *car = st->waiting[i];
        int p = -1, k = -1;
        if (QueueEmpty(st) && FreeSeat(l, st, &p, &k)) {
            ReserveSeat(st, car, p, k);
        } else if (QueueHasRoom(l, st)) {
            ReserveQueue(st, car);
        } else {
            break;
        }
        st->waiting[i] = st->waiting[--st->waiters];  // порядок в массиве не важен: решает ticket
        car->ticket = -1;
        if (l->cfg->sabotage == SAB_DEADLOCK && car->place == CAR_POST && car->stage >= 0 &&
            car->stage != st->index) {
            // намеренная ошибка: «заглянуть» на стадию, где стоит машина, не отпустив свою -
            // два мьютекса стадий сразу; LOCKDEP ловит это до блокировки
            Stage *other = &l->stage[car->stage];
            MutexLock(&other->m);
            MutexUnlock(&other->m);
        }
        SimWake(&car->th);
    }
}

void StageDispatch(Line *l, Stage *st) {
    SeatQueued(l, st);
    GrantWaiters(l, st);
}
