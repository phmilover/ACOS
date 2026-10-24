/**
 * @file stage.c
 * @brief Монитор стадии на мьютексе и условных переменных.
 *
 * Всё под мьютексом стадии. Бронь места на посту - это место с car = машина и left < 0:
 * оно уже не свободно, а post->reserved считает такие места. Бронь места в очереди считает
 * queueReserved, и она входит в проверку вместимости. Поэтому пока машина переезжает, её
 * будущее место никто не займёт (I5), а сама она числится только на старом месте (I2).
 *
 * Пробуждение условное (WakeWaiting): seatFree будят, только если в очереди есть машины и
 * свободное место на исправном посту, room - только если есть ожидающие и место для них. Так
 * предикат StageQueueTurn или StageRoomTurn хоть одного разбуженного истинен, а WakeWaiting
 * зовётся после каждого изменения, которое может сделать предикат истинным, - ждущий с
 * истинным предикатом не остаётся спать.
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

/// Сколько постов стадии ищет FreeSeat. С SAB_OVERFLOW - специально на один меньше (ошибка на
/// единицу в границе цикла; один пост ищется всегда): очередь перед стадией с двумя постами
/// заполняется на любом seed, и лишнее место из QueueLimit наверняка занимают.
static int SearchedPosts(const Line *l, int s) {
    int posts = l->cfg->stage[s].posts;
    return l->cfg->sabotage == SAB_OVERFLOW && posts > 1 ? posts - 1 : posts;
}

/// Первое свободное место на исправном посту. @return 1 и пост/место в *p, *k, иначе 0
static int FreeSeat(const Line *l, Stage *st, int *p, int *k) REQUIRES(st->m) {
    const StageSpec *spec = &l->cfg->stage[st->index];
    for (int i = 0; i < SearchedPosts(l, st->index); ++i) {
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

/// Какое место есть для машины снаружи: на посту (очередь пуста), иначе в очереди.
/// @return RES_SEAT (место в *p, *k), RES_QUEUE или RES_NONE
static Reservation FreeRoom(const Line *l, Stage *st, int *p, int *k) REQUIRES(st->m) {
    if (QueueEmpty(st) && FreeSeat(l, st, p, k)) {
        return RES_SEAT;
    }
    return QueueHasRoom(l, st) ? RES_QUEUE : RES_NONE;
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

/// Индекс машины в очереди стадии или -1.
static int QueueIndex(Stage *st, const Car *car) REQUIRES(st->m) {
    for (int i = 0; i < st->queued; ++i) {
        if (st->queue[i] == car->id) {
            return i;
        }
    }
    return -1;
}

/// Лучшая машина в очереди: больше оценка, при равной - кто раньше встал. @return индекс или -1
static int BestQueued(Line *l, Stage *st) REQUIRES(st->m) {
    int bi = -1, bscore = 0;
    for (int i = 0; i < st->queued; ++i) {
        const Car *car = LineCar(l, st->queue[i]);
        if (!SOFT_ASSERT(car != NULL, "unknown car in the queue")) {
            continue;
        }
        int score = StageScore(l, car);
        if (bi < 0 || score > bscore) {
            bi = i;
            bscore = score;
        }
    }
    return bi;
}

/// Индекс машины в списке ожидающих стадии или -1.
static int WaiterIndex(Stage *st, const Car *car) REQUIRES(st->m) {
    for (int i = 0; i < st->waiters; ++i) {
        if (st->waiting[i] == car) {
            return i;
        }
    }
    return -1;
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

/// Первый ожидающий. Список не пуст.
static int BestWaiter(const Line *l, Stage *st) REQUIRES(st->m) {
    int best = 0;
    for (int i = 1; i < st->waiters; ++i) {
        if (WaiterBefore(l, st->waiting[i], st->waiting[best])) {
            best = i;
        }
    }
    return best;
}

/// Будит машины очереди: SimGrant каждой, ждущей seatFree, затем CondBroadcast.
static void WakeQueue(Line *l, Stage *st) REQUIRES(st->m) {
    for (int i = 0; i < st->queued; ++i) {
        Car *car = LineCar(l, st->queue[i]);
        if (SOFT_ASSERT(car != NULL, "unknown car in the queue")) {
            SimGrant(&car->th, &st->seatFree);
        }
    }
    CondBroadcast(&st->seatFree);
}

/// Будит ожидающих места: SimGrant каждой машине из waiting[], ждущей room, затем CondBroadcast.
static void WakeRoom(Stage *st) REQUIRES(st->m) {
    for (int i = 0; i < st->waiters; ++i) {
        SimGrant(&st->waiting[i]->th, &st->room);
    }
    CondBroadcast(&st->room);
}

/// Будит тех, для кого есть место: машины очереди (свободное место на исправном посту) и
/// ожидающих (место для машины снаружи).
static void WakeWaiting(Line *l, Stage *st) REQUIRES(st->m) {
    int p = -1, k = -1;
    if (st->queued > 0 && FreeSeat(l, st, &p, &k)) {
        WakeQueue(l, st);
    }
    if (st->waiters > 0 && FreeRoom(l, st, &p, &k) != RES_NONE) {
        WakeRoom(st);
    }
}

void StageWakePost(Stage *st, Post *post) {
    for (int k = 0; k < post->capacity; ++k) {
        const Seat *seat = &post->seat[k];
        if (seat->car < 0 || seat->left != 0) {
            continue;  // свободно, бронь или операция идёт: ту машину разбудит конец её операции
        }
        Car *car = LineCar(post->line, seat->car);
        if (SOFT_ASSERT(car != NULL, "unknown car on a post")) {
            SimGrant(&car->th, &post->done);
        }
    }
    (void)st;  // мьютекс стадии нужен вызывающему: под ним меняют left и ждут done
    CondBroadcast(&post->done);
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

Admit StageEnter(Line *l, Stage *st, Car *car) {
    if (!SOFT_ASSERT(car->res == RES_NONE, "car already holds a reservation")) {
        return ADMIT_FULL;
    }
    if (st->waiters > 0) {
        return ADMIT_FULL;  // с улицы ожидающих не обгоняют
    }
    int p = -1, k = -1;
    Reservation r = FreeRoom(l, st, &p, &k);
    if (r == RES_NONE) {
        return ADMIT_FULL;
    }
    if (r == RES_SEAT) {
        ReserveSeat(st, car, p, k);
    } else {
        ReserveQueue(st, car);
    }
    StageCommit(l, st, car, -1);
    return r == RES_SEAT ? ADMIT_SEAT : ADMIT_QUEUE;
}

int StageQueueTurn(Line *l, Stage *st, const Car *car, int *post, int *seat) {
    int i = QueueIndex(st, car);
    return i >= 0 && BestQueued(l, st) == i && FreeSeat(l, st, post, seat);
}

SimStatus StageAwaitSeat(Line *l, Stage *st, Car *car) {
    int p = -1, k = -1;
    while (!StageQueueTurn(l, st, car, &p, &k)) {
        if (!SOFT_ASSERT(QueueIndex(st, car) >= 0, "car is not in the queue")) {
            return SIM_STOP;
        }
        WaitReason why = {.kind = WAIT_QUEUE, .stage = st->index, .post = -1};
        if (SimWaitOn(&car->th, &st->seatFree, &st->m, why) == SIM_STOP) {
            return SIM_STOP;
        }
    }
    int i = QueueIndex(st, car);
    for (int j = i; j + 1 < st->queued; ++j) {
        st->queue[j] = st->queue[j + 1];
    }
    st->queued--;
    StartOperation(l, st, car, p, k);
    WakeWaiting(l, st);  // место в очереди освободилось, «лучший» в очереди сменился
    return SIM_OK;
}

SimStatus StageAwaitOperation(Line *l, Stage *st, Car *car) {
    (void)l;
    for (;;) {
        if (!SOFT_ASSERT(car->place == CAR_POST && car->stage == st->index && car->post >= 0,
                         "car is not on a post of this stage")) {
            return SIM_STOP;
        }
        Post *post = &st->post[car->post];
        if (post->seat[car->seat].left == 0) {
            return SIM_OK;
        }
        WaitReason why = {.kind = post->repair > 0 ? WAIT_REPAIR : WAIT_OPERATION,
                          .stage = st->index,
                          .post = car->post};
        if (SimWaitOn(&car->th, &post->done, &st->m, why) == SIM_STOP) {
            return SIM_STOP;
        }
    }
}

Reservation StageRoomTurn(Line *l, Stage *st, const Car *car) {
    int i = WaiterIndex(st, car);
    if (i < 0 ? st->waiters > 0 : BestWaiter(l, st) != i) {
        return RES_NONE;
    }
    int p = -1, k = -1;
    return FreeRoom(l, st, &p, &k);
}

/// Ставит машину в список ожидающих стадии. @return 0 или -1, если список полон
static int AddWaiter(Line *l, Stage *st, Car *car) REQUIRES(st->m) {
    if (!SOFT_ASSERT(st->waiters < MAX_WAITERS, "waiting list is full")) {
        return -1;
    }
    car->ticket = atomic_fetch_add(&l->nextTicket, 1) + 1;
    st->waiting[st->waiters++] = car;
    return 0;
}

SimStatus StageReserve(Line *l, Stage *st, Car *car, int from, int fromPost) {
    if (car->res != RES_NONE && car->resStage == st->index) {
        return SIM_OK;  // бронь уже выдана
    }
    if (!SOFT_ASSERT(car->res == RES_NONE, "car already holds a reservation elsewhere")) {
        return SIM_STOP;
    }
    while (StageRoomTurn(l, st, car) == RES_NONE) {
        if (WaiterIndex(st, car) < 0) {
            if (AddWaiter(l, st, car) != 0) {
                return SIM_STOP;
            }
            EV(EV_BLOCKED, car->id, from, fromPost, car->program, st->index);
        }
        WaitReason why = {.kind = WAIT_ROOM, .stage = st->index, .post = -1};
        if (SimWaitOn(&car->th, &st->room, &st->m, why) == SIM_STOP) {
            return SIM_STOP;
        }
    }
    int p = -1, k = -1;
    if (FreeRoom(l, st, &p, &k) == RES_SEAT) {
        ReserveSeat(st, car, p, k);
    } else {
        ReserveQueue(st, car);
    }
    int i = WaiterIndex(st, car);
    if (i >= 0) {
        st->waiting[i] = st->waiting[--st->waiters];  // порядок в массиве не важен: решает ticket
        car->ticket = -1;
    }
    WakeWaiting(l, st);  // сменился первый ожидающий; место могло остаться
    return SIM_OK;
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
        // с SAB_OVERFLOW QueueLimit пропускает лишнюю машину, и это ловится здесь (как в ИЗ1)
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
    WakeWaiting(l, st);  // в очереди новая машина: она может оказаться лучшей при свободном месте
}

void StageMove(Line *l, Stage *from, Stage *to, Car *car) {
    MutexLock(&from->m);
    StageVacate(l, from, car);
    if (l->cfg->sabotage == SAB_DEADLOCK) {
        // намеренная ошибка: «переехать под одним замком» - мьютекс следующей стадии берётся,
        // пока текущая ещё захвачена. LOCKDEP ловит это до блокировки
        MutexLock(&to->m);
        StageCommit(l, to, car, from->index);
        MutexUnlock(&to->m);
        MutexUnlock(&from->m);
        return;
    }
    MutexUnlock(&from->m);
    MutexLock(&to->m);
    StageCommit(l, to, car, from->index);
    MutexUnlock(&to->m);
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
            WakeWaiting(l, st);
            return;
        }
    }
    SOFT_ASSERT(0, "car is not on a post of this stage");
}

void StageRepaired(Line *l, Stage *st) {
    const StageSpec *spec = &l->cfg->stage[st->index];
    for (int p = 0; p < spec->posts; ++p) {
        if (st->post[p].repair > 0) {
            continue;
        }
        for (int k = 0; k < spec->capacity; ++k) {
            const Seat *seat = &st->post[p].seat[k];
            if (seat->car < 0 || seat->left <= 0) {
                continue;  // свободно, бронь или операция уже кончилась
            }
            Car *car = LineCar(l, seat->car);
            if (SOFT_ASSERT(car != NULL, "unknown car on a post")) {
                SimSetReason(&car->th,
                             (WaitReason){.kind = WAIT_OPERATION, .stage = st->index, .post = p});
            }
        }
    }
    WakeWaiting(l, st);
}
