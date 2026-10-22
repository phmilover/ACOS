#include "sim.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif

#include "soft_assert.h"

#define DELAY_CHUNK_MS 20  // задержка -d идёт кусками, чтобы остановка не ждала её конца
#define MAX_STOP_PAIRS 64  // различных (условие, мьютекс) у ждущих: стадий 6 x (2 + 4 поста) + 1

static Mutex mu;                // мьютекс часов
static Cond quiet;              // часы ждут на ней покоя, SimKick или остановки
static atomic_int muReady = 0;  // мьютекс создан; SimKick читает из любого потока
static int quietReady = 0;      // условная переменная часов создана

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
// SimKick: крючок надо вызвать снова, даже если нового покоя не было (запрос сигнала)
static atomic_int kicked = 0;

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
/// Счётчик работающих растёт до пробуждения, поэтому часы не увидят ложного покоя.
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
        CondSignal(&t->cv);
    }
}

/// После того как поток перестал работать: запустить следующих, а если некого - разбудить часы.
static void AfterStop(void) REQUIRES(mu) {
    Dispatch();
    if (running == 0 && !readyHead) {
        ++quietSeq;
        CondSignal(&quiet);
    }
}

/// Ждёт на своей условной переменной, пока поток не станет работающим или модель не
/// остановится. @return SIM_STOP, если останавливается
static SimStatus AwaitRun(SimThread *self) REQUIRES(mu) {
    while (self->state != ST_RUNNING && !atomic_load(&stopping)) {
        CondWait(&self->cv, &mu);
    }
    return atomic_load(&stopping) ? SIM_STOP : SIM_OK;
}

int SimInit(const SimOptions *o) {
    if (!atomic_load(&muReady)) {
        MutexInit(&mu, RANK_SIM, "sim clock");
        atomic_store(&muReady, 1);
    }
    if (!quietReady) {
        if (CondInit(&quiet) != 0) {
            return -1;
        }
        quietReady = 1;
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
    atomic_store(&kicked, 0);
    opt = *o;
    return 0;
}

int SimThreadInit(SimThread *t, ThreadKind kind, int id, const char *name, int order) {
    memset(t, 0, sizeof *t);
    t->kind = kind;
    t->id = id;
    snprintf(t->name, sizeof t->name, "%s", name ? name : "?");
    t->order = order;
    t->state = ST_DONE;
    t->why.kind = WAIT_NONE;
    if (CondInit(&t->cv) != 0) {
        return -1;
    }
    MutexLock(&mu);
    t->live = 1;
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
    if (!t->live) {
        MutexUnlock(&mu);
        return;  // уже уничтожен (Reap, затем LineFree): cond второй раз не уничтожаем
    }
    t->live = 0;
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
    CondDestroy(&t->cv);
}

void SimSpawned(SimThread *t) {
    MutexLock(&mu);
    if (atomic_load(&stopping)) {
        t->state = ST_READY;  // SimBegin увидит остановку и сразу вернётся
    } else {
        Enqueue(t);
        Dispatch();
    }
    MutexUnlock(&mu);
}

/// Системный номер вызывающего потока: для панели потоков, не для логики. 0 - неизвестен.
static long SystemThreadId(void) {
#ifdef __linux__
    return (long)syscall(SYS_gettid);
#elif defined(__APPLE__)
    uint64_t id = 0;
    return pthread_threadid_np(NULL, &id) == 0 ? (long)id : 0;
#else
    return 0;
#endif
}

SimStatus SimBegin(SimThread *self) {
    SOFT_ASSERT(LockdepHeldCount() == 0, "SimBegin with a lock held");
    LockdepSetThreadName(self->name);
    MutexLock(&mu);
    self->sysId = SystemThreadId();
    SimStatus st = AwaitRun(self);
    MutexUnlock(&mu);
    return st;
}

void SimExit(SimThread *self) {
    MutexLock(&mu);
    if (self->state == ST_RUNNING) {
        --running;
    } else if (self->state == ST_READY) {
        Unqueue(self);  // например, поток не создался и создатель выходит за него
    }
    self->state = ST_DONE;
    if (!atomic_load(&stopping)) {
        AfterStop();
    }
    MutexUnlock(&mu);
}

SimStatus SimSleep(SimThread *self, int minutes) {
    SOFT_ASSERT(LockdepHeldCount() == 0, "SimSleep with a lock held");
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
    SimStatus st = AwaitRun(self);
    MutexUnlock(&mu);
    return st;
}

SimStatus SimWaitOn(SimThread *self, Cond *c, Mutex *m, WaitReason why) {
    SOFT_ASSERT(LockdepHeldCount() == 1, "SimWaitOn must hold only the monitor mutex");
    self->granted = 0;
    MutexLock(&mu);
    if (atomic_load(&stopping)) {
        MutexUnlock(&mu);
        return SIM_STOP;
    }
    self->state = ST_PARKED;
    self->why = why;
    self->waitCond = c;
    self->waitMutex = m;
    ++self->parks;
    ++totalParks;
    --running;
    AfterStop();
    MutexUnlock(&mu);

    // SimStop ставит флаг до того, как возьмёт m и разбудит c, поэтому проверка под m надёжна
    while (!self->granted && !atomic_load(&stopping)) {
        CondWait(c, m);
    }

    MutexLock(&mu);
    self->waitCond = NULL;
    self->waitMutex = NULL;
    // разрешённый, но ещё не запущенный (очередь -D или разрешили во время крючка) ждёт запуска
    int needRun = self->granted && self->state != ST_RUNNING;
    MutexUnlock(&mu);
    if (needRun) {
        // ждём без m: иначе держатель жетона (или крючок) мог бы навсегда встать на m
        MutexUnlock(m);
        MutexLock(&mu);
        AwaitRun(self);
        MutexUnlock(&mu);
        MutexLock(m);
    }
    return atomic_load(&stopping) ? SIM_STOP : SIM_OK;
}

int SimGrant(SimThread *t, const Cond *c) {
    MutexLock(&mu);
    int ok = !atomic_load(&stopping) && t->state == ST_PARKED && t->waitCond == c;
    if (ok) {
        t->granted = 1;  // под waitMutex: его держит вызывающий
        Enqueue(t);
        Dispatch();
    }
    MutexUnlock(&mu);
    return ok;
}

void SimSetReason(SimThread *t, WaitReason why) {
    MutexLock(&mu);
    if (t->state == ST_PARKED) {
        t->why = why;
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
    // мьютексы мониторов рангом ниже часов: брать их под мьютексом часов нельзя, поэтому пары
    // собираются здесь, а будятся после
    Cond *conds[MAX_STOP_PAIRS];
    Mutex *mutexes[MAX_STOP_PAIRS];
    int n = 0;
    MutexLock(&mu);
    if (!atomic_exchange(&stopping, 1)) {
        for (SimThread *t = allHead; t; t = t->nextAll) {
            CondSignal(&t->cv);
            if (!t->waitCond) {
                continue;
            }
            int known = 0;
            for (int i = 0; i < n && !known; ++i) {
                known = conds[i] == t->waitCond && mutexes[i] == t->waitMutex;
            }
            if (!known && SOFT_ASSERT(n < MAX_STOP_PAIRS, "SimStop: too many wait conditions")) {
                conds[n] = (Cond *)t->waitCond;  // const только для SimGrant: там c не меняют
                mutexes[n] = t->waitMutex;
                ++n;
            }
        }
        CondSignal(&quiet);
    }
    MutexUnlock(&mu);
    for (int i = 0; i < n; ++i) {
        MutexLock(mutexes[i]);
        CondBroadcast(conds[i]);
        MutexUnlock(mutexes[i]);
    }
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

static long MonotonicMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((long)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

/// Задержка темпа показа на условной переменной часов кусками по DELAY_CHUNK_MS: её прерывают
/// SimKick и SimStop. Остаток считается по монотонным часам, но сам CondTimedWaitMs берёт срок
/// от CLOCK_REALTIME: скачок времени назад внутри куска растянет этот кусок, не больше.
/// @return 1, если задержку прервал SimKick (крючок надо вызвать снова), иначе 0
static int Delay(int ms) {
    long end = MonotonicMs() + ms;
    int kick = 0;
    MutexLock(&mu);
    while (!atomic_load(&stopping)) {
        if (atomic_exchange(&kicked, 0)) {
            kick = 1;
            break;
        }
        long left = end - MonotonicMs();
        if (left <= 0) {
            break;
        }
        CondTimedWaitMs(&quiet, &mu, left < DELAY_CHUNK_MS ? (int)left : DELAY_CHUNK_MS);
    }
    MutexUnlock(&mu);
    return kick;
}

void *SimClockMain(void *unused) {
    (void)unused;
    LockdepSetThreadName("clock");
    if (opt.start) {
        BarrierWait(opt.start);
    }
    long seen = -1;  // какой момент покоя уже обработан: крючок на один покой - один раз
    MutexLock(&mu);
    for (;;) {
        // тот же покой обрабатываем снова только по SimKick
        while (!atomic_load(&stopping) &&
               (running > 0 || readyHead || (quietSeq == seen && !atomic_load(&kicked)))) {
            CondWait(&quiet, &mu);
        }
        if (atomic_load(&stopping)) {
            break;
        }
        seen = quietSeq;
        int at = now, next = NextWake();
        held = 1;  // кто разрешит выход потоку извне во время крючка - встанет в очередь
        MutexUnlock(&mu);

        int go;
        do {  // SimKick во время задержки: крючок ещё раз, в ту же минуту (покой не нарушен)
            atomic_store(&kicked, 0);
            go = opt.hook ? opt.hook(at, next, opt.ctx) : next != -1;
        } while (go && next != -1 && Delay(opt.delayMs));

        MutexLock(&mu);
        held = 0;
        if (!go || atomic_load(&stopping)) {
            MutexUnlock(&mu);
            SimStop();
            return NULL;
        }
        if (readyHead) {
            // покой нарушен извне: время не двигаем, дождёмся следующего покоя
            Dispatch();
        } else if (next != -1) {
            now = next;
            WakeDue();
            Dispatch();
        }
    }
    MutexUnlock(&mu);
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

void SimKick(void) {
    atomic_store(&kicked, 1);
    if (!atomic_load(&muReady)) {
        return;  // SimInit ещё не звали: будить некого
    }
    MutexLock(&mu);  // сигнал под мьютексом: часы не пропустят его между проверкой и ожиданием
    CondSignal(&quiet);
    MutexUnlock(&mu);
}
