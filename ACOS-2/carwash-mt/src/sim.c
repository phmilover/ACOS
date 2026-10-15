#include "sim.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "mutex.h"
#include "soft_assert.h"

#define DELAY_CHUNK_MS 20  // задержка -d идёт кусками, чтобы остановка не ждала её конца

static Mutex mu;         // мьютекс часов
static int muReady = 0;  // мьютекс создан (SimInit зовёт только главный поток)

static int now GUARDED_BY(mu) = 0;
static int running GUARDED_BY(mu) = 0;              // сколько потоков модели работает
static SimThread *readyHead GUARDED_BY(mu) = NULL;  // очередь запуска (FIFO)
static SimThread *readyTail GUARDED_BY(mu) = NULL;
static SimThread *allHead GUARDED_BY(mu) = NULL;  // все зарегистрированные потоки
static SimThread *allTail GUARDED_BY(mu) = NULL;
static int held GUARDED_BY(mu) = 0;       // идёт крючок или задержка: никого не запускать
static long quietSeq GUARDED_BY(mu) = 0;  // номер последнего момента покоя
static long totalParks GUARDED_BY(mu) = 0;

static atomic_int stopping = 0;
static SimOptions opt;  // пишется в SimInit до создания потоков, дальше только читается
static Sem clockSem;    // часы ждут на нём момента покоя или остановки

/// Ставит поток в конец очереди запуска.
static void Enqueue(SimThread *t) REQUIRES(mu) {
    t->state = ST_READY;
    t->nextReady = NULL;
    if (readyTail) {
        readyTail->nextReady = t;
    } else {
        readyHead = t;
    }
    readyTail = t;
}

/// Убирает поток из очереди запуска, если он там есть.
static void Unqueue(SimThread *t) REQUIRES(mu) {
    SimThread *prev = NULL;
    for (SimThread *p = readyHead; p; prev = p, p = p->nextReady) {
        if (p != t) {
            continue;
        }
        if (prev) {
            prev->nextReady = p->nextReady;
        } else {
            readyHead = p->nextReady;
        }
        if (readyTail == p) {
            readyTail = prev;
        }
        p->nextReady = NULL;
        return;
    }
}

/// Запускает потоки из очереди: в обычном режиме всех, в -D - голову, если никто не работает.
/// Счётчик работающих растёт до SemPost, поэтому часы не увидят ложного покоя.
static void Dispatch(void) REQUIRES(mu) {
    if (held || atomic_load(&stopping)) {
        return;
    }
    while (readyHead && (!opt.deterministic || running == 0)) {
        SimThread *t = readyHead;
        readyHead = t->nextReady;
        if (!readyHead) {
            readyTail = NULL;
        }
        t->nextReady = NULL;
        t->state = ST_RUNNING;
        ++running;
        SemPost(&t->wake);
    }
}

/// После того как поток перестал работать: запустить следующих, а если некого - разбудить часы.
static void AfterStop(void) REQUIRES(mu) {
    Dispatch();
    if (running == 0 && !readyHead) {
        ++quietSeq;
        SemPost(&clockSem);
    }
}

/// Ждёт своего семафора. Вход с захваченным мьютексом часов, выход - без него.
static SimStatus Block(SimThread *self) RELEASE(mu) {
    MutexUnlock(&mu);
    SemWait(&self->wake);
    // захват после пробуждения даёт happens-before с будившим (и для TSan)
    MutexLock(&mu);
    SimStatus st = atomic_load(&stopping) ? SIM_STOP : SIM_OK;
    MutexUnlock(&mu);
    return st;
}

int SimInit(const SimOptions *o) {
    if (!muReady) {
        MutexInit(&mu, RANK_SIM, "sim clock");
        muReady = 1;
    }
    MutexLock(&mu);
    now = 0;
    running = 0;
    readyHead = readyTail = NULL;
    allHead = allTail = NULL;
    held = 0;
    quietSeq = 0;
    totalParks = 0;
    MutexUnlock(&mu);
    atomic_store(&stopping, 0);
    opt = *o;
    SemClose(&clockSem);
    // начальное 1: часы сразу проверяют покой (модель могла начаться без потоков)
    return SemOpen(&clockSem, 1);
}

int SimThreadInit(SimThread *t, ThreadKind kind, int id, const char *name, int order) {
    memset(t, 0, sizeof *t);
    t->kind = kind;
    t->id = id;
    snprintf(t->name, sizeof t->name, "%s", name ? name : "?");
    t->order = order;
    t->state = ST_DONE;
    t->why.kind = WAIT_NONE;
    if (SemOpen(&t->wake, 0) != 0) {
        return -1;
    }
    MutexLock(&mu);
    if (allTail) {
        allTail->nextAll = t;
    } else {
        allHead = t;
    }
    allTail = t;
    MutexUnlock(&mu);
    return 0;
}

void SimThreadDestroy(SimThread *t) {
    MutexLock(&mu);
    SOFT_ASSERT(t->state == ST_DONE, "SimThreadDestroy of a live thread");
    SimThread *prev = NULL;
    for (SimThread *p = allHead; p; prev = p, p = p->nextAll) {
        if (p != t) {
            continue;
        }
        if (prev) {
            prev->nextAll = p->nextAll;
        } else {
            allHead = p->nextAll;
        }
        if (allTail == p) {
            allTail = prev;
        }
        break;
    }
    t->nextAll = NULL;
    MutexUnlock(&mu);
    SemClose(&t->wake);
}

void SimSpawned(SimThread *t) {
    MutexLock(&mu);
    t->wakePending = 0;
    if (atomic_load(&stopping)) {
        t->state = ST_READY;  // SimBegin увидит остановку и сразу вернётся
    } else {
        Enqueue(t);
        Dispatch();
    }
    MutexUnlock(&mu);
}

SimStatus SimBegin(SimThread *self) {
    SOFT_ASSERT(LockdepHeldCount() == 0, "SimBegin with a mutex held");
    LockdepSetThreadName(self->name);
    MutexLock(&mu);
    if (atomic_load(&stopping)) {
        MutexUnlock(&mu);
        return SIM_STOP;
    }
    return Block(self);
}

void SimExit(SimThread *self) {
    MutexLock(&mu);
    if (self->state == ST_RUNNING) {
        --running;
    } else if (self->state == ST_READY) {
        Unqueue(self);  // например, pthread_create не удался и создатель выходит за поток
    }
    self->state = ST_DONE;
    self->wakePending = 0;
    if (!atomic_load(&stopping)) {
        AfterStop();
    }
    MutexUnlock(&mu);
}

SimStatus SimSleep(SimThread *self, int minutes) {
    SOFT_ASSERT(LockdepHeldCount() == 0, "SimSleep with a mutex held");
    if (!SOFT_ASSERT(minutes >= 1, "SimSleep needs at least one minute")) {
        minutes = 1;
    }
    MutexLock(&mu);
    if (atomic_load(&stopping)) {
        MutexUnlock(&mu);
        return SIM_STOP;
    }
    self->state = ST_SLEEPING;
    self->wakeAt = now + minutes;
    self->why = (WaitReason){.kind = WAIT_TIME, .stage = -1, .post = -1};
    --running;
    AfterStop();
    return Block(self);
}

SimStatus SimPark(SimThread *self, WaitReason why) {
    SOFT_ASSERT(LockdepHeldCount() == 0, "SimPark with a mutex held");
    MutexLock(&mu);
    if (atomic_load(&stopping)) {
        MutexUnlock(&mu);
        return SIM_STOP;
    }
    if (self->wakePending) {  // разбудили раньше, чем уснул
        self->wakePending = 0;
        MutexUnlock(&mu);
        return SIM_OK;
    }
    self->state = ST_PARKED;
    self->why = why;
    ++self->parks;
    ++totalParks;
    --running;
    AfterStop();
    return Block(self);
}

void SimSetReason(SimThread *t, WaitReason why) {
    MutexLock(&mu);
    t->why = why;
    MutexUnlock(&mu);
}

void SimWake(SimThread *t) {
    MutexLock(&mu);
    if (!atomic_load(&stopping)) {
        if (t->state == ST_PARKED) {
            Enqueue(t);
            Dispatch();
        } else if (t->state == ST_RUNNING) {
            t->wakePending = 1;
        }
    }
    MutexUnlock(&mu);
}

int SimNow(void) {
    MutexLock(&mu);
    int n = now;
    MutexUnlock(&mu);
    return n;
}

void SimStop(void) {
    MutexLock(&mu);
    if (!atomic_exchange(&stopping, 1)) {
        // работающие увидят флаг при следующем вызове Sim*, ждущих будим
        for (SimThread *t = allHead; t; t = t->nextAll) {
            if (t->state == ST_READY || t->state == ST_SLEEPING || t->state == ST_PARKED) {
                SemPost(&t->wake);
            }
        }
        SemPost(&clockSem);
    }
    MutexUnlock(&mu);
}

int SimStopping(void) {
    return atomic_load(&stopping);
}

/// @return ближайшая минута пробуждения спящих или -1
static int NextWake(void) REQUIRES(mu) {
    int next = -1;
    for (SimThread *t = allHead; t; t = t->nextAll) {
        if (t->state == ST_SLEEPING && (next == -1 || t->wakeAt < next)) {
            next = t->wakeAt;
        }
    }
    return next;
}

/// Ставит в очередь всех, кому пора проснуться в минуту now, по возрастанию order.
static void WakeDue(void) REQUIRES(mu) {
    SimThread *sorted = NULL;
    for (SimThread *t = allHead; t; t = t->nextAll) {
        if (t->state != ST_SLEEPING || t->wakeAt != now) {
            continue;
        }
        SimThread **p = &sorted;  // вставка после равных: порядок регистрации сохраняется
        while (*p && (*p)->order <= t->order) {
            p = &(*p)->nextReady;
        }
        t->nextReady = *p;
        *p = t;
    }
    while (sorted) {
        SimThread *next = sorted->nextReady;
        Enqueue(sorted);
        sorted = next;
    }
}

/// Задержка темпа показа кусками по DELAY_CHUNK_MS с проверкой остановки.
static void Delay(int ms) {
    while (ms > 0 && !SimStopping()) {
        int step = ms < DELAY_CHUNK_MS ? ms : DELAY_CHUNK_MS;
        struct timespec ts = {.tv_sec = 0, .tv_nsec = (long)step * 1000000L};
        nanosleep(&ts, NULL);
        ms -= step;
    }
}

void *SimClockMain(void *unused) {
    (void)unused;
    LockdepSetThreadName("clock");
    long seen = -1;  // какой момент покоя уже обработан: лишние SemPost не зовут крючок дважды
    for (;;) {
        SemWait(&clockSem);
        MutexLock(&mu);
        if (atomic_load(&stopping)) {
            MutexUnlock(&mu);
            break;
        }
        if (running > 0 || readyHead || quietSeq == seen) {
            MutexUnlock(&mu);
            continue;
        }
        seen = quietSeq;
        int at = now, next = NextWake();
        held = 1;  // кто разбудит поток извне во время крючка - встанет в очередь
        MutexUnlock(&mu);

        int go = opt.hook ? opt.hook(at, next, opt.ctx) : next != -1;
        if (go && next != -1) {
            Delay(opt.delayMs);
        }

        MutexLock(&mu);
        held = 0;
        if (!go || atomic_load(&stopping)) {
            MutexUnlock(&mu);
            SimStop();
            break;
        }
        if (readyHead) {
            // покой нарушен извне: время не двигаем, дождёмся следующего покоя
            Dispatch();
        } else if (next != -1) {
            now = next;
            WakeDue();
            Dispatch();
        }
        MutexUnlock(&mu);
    }
    return NULL;
}

void SimForEach(void (*fn)(const SimThread *t, void *ctx), void *ctx) {
    MutexLock(&mu);
    for (const SimThread *t = allHead; t; t = t->nextAll) {
        fn(t, ctx);
    }
    MutexUnlock(&mu);
}

long SimTotalParks(void) {
    MutexLock(&mu);
    long n = totalParks;
    MutexUnlock(&mu);
    return n;
}
