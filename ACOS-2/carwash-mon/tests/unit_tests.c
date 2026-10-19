#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "barrier.h"
#include "config.h"
#include "rng.h"
#include "sim.h"
#include "soft_assert.h"
#include "sync.h"
#include "thread.h"
#include "unit.h"

static void TestStageCapacity(void) {
    Config c;
    char err[256];
    ConfigDefaults(&c);
    CHECK(ConfigParseText(&c, "stage = dry 2 3 5 3 2\nstage = wax 1 4 6 2\nprogram = P 1 1 dry\n",
                          err, sizeof err) == 0);
    CHECK(c.stage[0].capacity == 2);
    CHECK(c.stage[1].capacity == 1);  // шестое поле необязательно
}

/// Каждая строка плохая только ёмкостью, постами или лишним полем: маршрут у всех верный.
static void TestBadCapacity(void) {
    const char *bad[] = {
        "stage = a 1 1 2 1 0\nprogram = P 1 1 a\n",    // ёмкость 0
        "stage = a 1 1 2 1 5\nprogram = P 1 1 a\n",    // ёмкость больше MAX_CAPACITY
        "stage = a 5 1 2 1\nprogram = P 1 1 a\n",      // постов больше MAX_POSTS
        "stage = a 1 1 2 1 1 9\nprogram = P 1 1 a\n",  // лишнее седьмое поле
    };
    for (int i = 0; i < 4; ++i) {
        Config c;
        char err[256];
        ConfigDefaults(&c);
        int rc = ConfigParseText(&c, bad[i], err, sizeof err);
        if (rc == 0) {
            rc = ConfigValidate(&c, err, sizeof err);
        }
        CHECK(rc == -1);
    }
    // контроль: та же форма с верной ёмкостью проходит оба шага
    Config c;
    char err[256];
    ConfigDefaults(&c);
    CHECK(ConfigParseText(&c, "stage = a 1 1 2 1 2\nprogram = P 1 1 a\n", err, sizeof err) == 0);
    CHECK(ConfigValidate(&c, err, sizeof err) == 0);
}

static void TestRngStreams(void) {
    Rng a, b, c;
    RngInit(&a, 42, 1000);
    RngInit(&b, 42, 1000);
    RngInit(&c, 42, 1001);
    int same = 1, diff = 0;
    for (int i = 0; i < 100; ++i) {
        int x = RngRange(&a, 0, 1000), y = RngRange(&b, 0, 1000), z = RngRange(&c, 0, 1000);
        same = same && x == y;
        diff = diff || x != z;
        CHECK(x >= 0 && x <= 1000);
    }
    CHECK(same);
    CHECK(diff);
}

static void TestSabotageNames(void) {
    Sabotage s;
    CHECK(ConfigParseSabotage("race", &s) == 0 && s == SAB_RACE);
    CHECK(ConfigParseSabotage("deadlock", &s) == 0 && s == SAB_DEADLOCK);
    CHECK(ConfigParseSabotage("overflow", &s) == 0 && s == SAB_OVERFLOW);
    CHECK(ConfigParseSabotage("boom", &s) == -1);
}

static void TestLockdepCatchesNesting(void) {
    LockdepSetFatal(0);
    Mutex a, b, bus;
    MutexInit(&a, RANK_STAGE, "stage A");
    MutexInit(&b, RANK_STAGE, "stage B");
    MutexInit(&bus, RANK_BUS, "bus");
    long before = LockdepViolations();
    MutexLock(&a);
    MutexLock(&bus);
    MutexUnlock(&bus);
    MutexUnlock(&a);  // по рангу - можно
    CHECK(LockdepViolations() == before);
    MutexLock(&a);
    MutexLock(&b);
    MutexUnlock(&b);
    MutexUnlock(&a);  // два мьютекса стадий
    CHECK(LockdepViolations() == before + 1);
    CHECK(LockdepHeldCount() == 0);
    MutexDestroy(&a);
    MutexDestroy(&b);
    MutexDestroy(&bus);
}

static long NowMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((long)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

static void SleepMs(int ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

// ---- Cond ----

typedef struct {
    Mutex m;
    Cond cv;
    int flag;
} CondCtx;

static void *CondSignaller(void *arg) {
    CondCtx *c = arg;
    SleepMs(50);
    MutexLock(&c->m);
    c->flag = 1;
    CondSignal(&c->cv);
    MutexUnlock(&c->m);
    return NULL;
}

static void TestCondWaitKeepsLockdep(void) {
    CondCtx c = {.flag = 0};
    MutexInit(&c.m, RANK_SIM, "cond test");
    CHECK(CondInit(&c.cv) == 0);
    pthread_t t;
    CHECK(ThreadCreate(&t, CondSignaller, &c, 0) == 0);
    MutexLock(&c.m);
    while (!c.flag) {
        CondWait(&c.cv, &c.m);
    }
    CHECK(LockdepHeldCount() == 1);
    MutexUnlock(&c.m);
    CHECK(LockdepHeldCount() == 0);
    pthread_join(t, NULL);
    CondDestroy(&c.cv);
    MutexDestroy(&c.m);
}

static void TestCondTimedWait(void) {
    Mutex m;
    Cond cv;
    MutexInit(&m, RANK_SIM, "timed test");
    CHECK(CondInit(&cv) == 0);
    MutexLock(&m);
    long t0 = NowMs();
    CHECK(CondTimedWaitMs(&cv, &m, 30) == 1);
    CHECK(NowMs() - t0 >= 25);
    CHECK(LockdepHeldCount() == 1);
    MutexUnlock(&m);
    CondDestroy(&cv);
    MutexDestroy(&m);
}

// ---- RwLock ----

typedef struct {
    RwLock l;
    atomic_int flag;
    atomic_int seenFlag;
    atomic_int readers;
    atomic_long releasedAt, wroteAt;
} RwCtx;

static void *RwSetter(void *arg) {
    RwCtx *c = arg;
    RwLockRead(&c->l);
    atomic_store(&c->flag, 1);
    SleepMs(50);
    RwLockUnlock(&c->l);
    return NULL;
}

static void TestRwLockShared(void) {
    RwCtx c;
    c.flag = 0;
    c.seenFlag = 0;
    CHECK(RwLockInit(&c.l, RANK_REGISTRY, "rw shared") == 0);
    pthread_t t;
    RwLockRead(&c.l);
    CHECK(ThreadCreate(&t, RwSetter, &c, 0) == 0);
    // читатель B пришёл, пока A держит чтение: если бы чтение было исключающим, флага не будет
    long t0 = NowMs();
    while (!atomic_load(&c.flag) && NowMs() - t0 < 1000) {
        SleepMs(1);
    }
    c.seenFlag = atomic_load(&c.flag);
    RwLockUnlock(&c.l);
    pthread_join(t, NULL);
    CHECK(c.seenFlag == 1);
    CHECK(LockdepHeldCount() == 0);
    RwLockDestroy(&c.l);
}

static void *RwWriter(void *arg) {
    RwCtx *c = arg;
    RwLockWrite(&c->l);
    atomic_store(&c->wroteAt, NowMs());
    RwLockUnlock(&c->l);
    return NULL;
}

static void TestRwLockExcludesWriter(void) {
    RwCtx c;
    c.releasedAt = 0;
    c.wroteAt = 0;
    CHECK(RwLockInit(&c.l, RANK_REGISTRY, "rw excl") == 0);
    pthread_t t;
    RwLockRead(&c.l);
    CHECK(ThreadCreate(&t, RwWriter, &c, 0) == 0);
    SleepMs(100);
    CHECK(atomic_load(&c.wroteAt) == 0);  // писатель всё ещё ждёт
    atomic_store(&c.releasedAt, NowMs());
    RwLockUnlock(&c.l);
    pthread_join(t, NULL);
    CHECK(atomic_load(&c.wroteAt) >= atomic_load(&c.releasedAt));
    RwLockDestroy(&c.l);
}

static void TestRwLockLockdep(void) {
    LockdepSetFatal(0);
    Mutex stage;
    RwLock reg;
    MutexInit(&stage, RANK_STAGE, "stage");
    CHECK(RwLockInit(&reg, RANK_REGISTRY, "registry") == 0);
    long before = LockdepViolations();
    MutexLock(&stage);
    RwLockRead(&reg);
    CHECK(LockdepHeldCount() == 2);
    RwLockUnlock(&reg);
    MutexUnlock(&stage);
    CHECK(LockdepViolations() == before);
    RwLockRead(&reg);
    MutexLock(&stage);  // ранг ниже удерживаемого чтения
    MutexUnlock(&stage);
    RwLockUnlock(&reg);
    CHECK(LockdepViolations() == before + 1);
    CHECK(LockdepHeldCount() == 0);
    RwLockDestroy(&reg);
    MutexDestroy(&stage);
}

// ---- Barrier ----

#define BAR_THREADS 5

typedef struct {
    Barrier b;
    atomic_int passed;
    atomic_int serial;
} BarCtx;

static void *BarWorker(void *arg) {
    BarCtx *c = arg;
    int last = BarrierWait(&c->b);
    atomic_fetch_add(&c->passed, 1);
    atomic_fetch_add(&c->serial, last);
    return NULL;
}

static void TestBarrierReleasesAll(void) {
    BarCtx c;
    c.passed = 0;
    c.serial = 0;
    CHECK(BarrierInit(&c.b, BAR_THREADS + 1) == 0);
    pthread_t t[BAR_THREADS];
    for (int i = 0; i < BAR_THREADS; ++i) {
        CHECK(ThreadCreate(&t[i], BarWorker, &c, 0) == 0);
    }
    SleepMs(100);
    CHECK(atomic_load(&c.passed) == 0);
    atomic_fetch_add(&c.serial, BarrierWait(&c.b));
    for (int i = 0; i < BAR_THREADS; ++i) {
        pthread_join(t[i], NULL);
    }
    CHECK(atomic_load(&c.passed) == BAR_THREADS);
    CHECK(atomic_load(&c.serial) == 1);
    BarrierDestroy(&c.b);
    CHECK(BarrierKind() != NULL);
}

#define ROUND_THREADS 3
#define ROUNDS        100

typedef struct {
    Barrier b;
    atomic_int counter;
    atomic_int bad;
} RoundCtx;

static void *RoundWorker(void *arg) {
    RoundCtx *c = arg;
    for (int r = 0; r < ROUNDS; ++r) {
        atomic_fetch_add(&c->counter, 1);
        BarrierWait(&c->b);
        // все трое уже прибавили; следующий круг начнётся только после второго барьера
        if (atomic_load(&c->counter) % ROUND_THREADS != 0) {
            atomic_fetch_add(&c->bad, 1);
        }
        BarrierWait(&c->b);
    }
    return NULL;
}

static void TestBarrierReusable(void) {
    RoundCtx c;
    c.counter = 0;
    c.bad = 0;
    CHECK(BarrierInit(&c.b, ROUND_THREADS) == 0);
    pthread_t t[ROUND_THREADS];
    for (int i = 0; i < ROUND_THREADS; ++i) {
        CHECK(ThreadCreate(&t[i], RoundWorker, &c, 0) == 0);
    }
    for (int i = 0; i < ROUND_THREADS; ++i) {
        pthread_join(t[i], NULL);
    }
    CHECK(atomic_load(&c.bad) == 0);
    CHECK(atomic_load(&c.counter) == ROUND_THREADS * ROUNDS);
    BarrierDestroy(&c.b);
}

// ---- Виртуальные часы ----

#define MAX_WORKERS 32
#define MAX_HOOKS   64

/// Поток-«пост»: count раз спит по step минут, после каждого сна записывает SimNow().
typedef struct {
    SimThread th;
    int step, count;
    int seen[8];
    int *log;  // общий журнал id (только в -D, без мьютекса)
    int *logLen;
    _Atomic int *busy;  // сколько потоков сейчас между сном и сном (в -D не больше 1)
    _Atomic int *maxBusy;
    SimStatus last;
} Worker;

static void *WorkerMain(void *arg) {
    Worker *w = arg;
    w->last = SimBegin(&w->th);
    for (int i = 0; w->last == SIM_OK && i < w->count; ++i) {
        w->last = SimSleep(&w->th, w->step);
        if (w->last != SIM_OK) {
            break;
        }
        w->seen[i] = SimNow();
        if (w->busy) {
            int b = atomic_fetch_add(w->busy, 1) + 1;
            if (b > atomic_load(w->maxBusy)) {
                atomic_store(w->maxBusy, b);
            }
            SleepMs(1);  // окно, чтобы поймать второго работающего
        }
        if (w->log) {
            w->log[(*w->logLen)++] = w->th.id;
        }
        if (w->busy) {
            atomic_fetch_sub(w->busy, 1);
        }
    }
    SimExit(&w->th);
    return NULL;
}

/// Что видел крючок: моменты покоя, ближайшие пробуждения, нарушения покоя.
typedef struct {
    int calls;
    int now[MAX_HOOKS], next[MAX_HOOKS];
    int notQuiet;                    // потоки в RUNNING/READY, замеченные через SimForEach
    int seen;                        // сколько потоков обошёл SimForEach за последний вызов
    const SimThread *watch;          // за каким потоком следить (NULL - ни за каким)
    SimState watchState[MAX_HOOKS];  // его состояние в каждом покое
} HookLog;

static void CountQuiet(const SimThread *t, void *ctx) {
    HookLog *h = ctx;
    ++h->seen;
    if (t->state == ST_RUNNING || t->state == ST_READY) {
        ++h->notQuiet;
    }
    if (t == h->watch && h->calls <= MAX_HOOKS) {
        h->watchState[h->calls - 1] = t->state;
    }
}

static int RecordHook(int now, int nextWake, void *ctx) {
    HookLog *h = ctx;
    if (h->calls < MAX_HOOKS) {
        h->now[h->calls] = now;
        h->next[h->calls] = nextWake;
    }
    ++h->calls;
    h->seen = 0;
    SimForEach(CountQuiet, h);
    return nextWake != -1;
}

/// Готовит модель, порождает потоки (часы последними), ждёт конца и всё убирает.
static void RunWorkers(Worker *w, int n, int deterministic, HookLog *h) {
    SimOptions o = {.deterministic = deterministic, .delayMs = 0, .hook = RecordHook, .ctx = h};
    CHECK(SimInit(&o) == 0);
    pthread_t tid[MAX_WORKERS], clock;
    for (int i = 0; i < n; ++i) {
        CHECK(SimThreadInit(&w[i].th, TH_POST, w[i].th.id, "post", w[i].th.order) == 0);
        SimSpawned(&w[i].th);
        CHECK(ThreadCreate(&tid[i], WorkerMain, &w[i], 0) == 0);
    }
    CHECK(ThreadCreate(&clock, SimClockMain, NULL, 0) == 0);
    pthread_join(clock, NULL);
    for (int i = 0; i < n; ++i) {
        pthread_join(tid[i], NULL);
        SimThreadDestroy(&w[i].th);
    }
}

static void TestSleepersWakeOnTime(void) {
    Worker w[2] = {{.th = {.id = 0}, .step = 3, .count = 1},
                   {.th = {.id = 1}, .step = 5, .count = 1}};
    HookLog h = {0};
    RunWorkers(w, 2, 0, &h);
    CHECK(w[0].seen[0] == 3);
    CHECK(w[1].seen[0] == 5);
    CHECK(w[0].last == SIM_OK && w[1].last == SIM_OK);
    CHECK(h.calls == 3);
    CHECK(h.now[0] == 0 && h.next[0] == 3);
    CHECK(h.now[1] == 3 && h.next[1] == 5);
    CHECK(h.now[2] == 5 && h.next[2] == -1);
}

/// Много потоков в обычном режиме: каждый видит ровно свои моменты пробуждения.
static void TestManySleepersParallel(void) {
    static Worker w[MAX_WORKERS];
    memset(w, 0, sizeof w);
    for (int i = 0; i < MAX_WORKERS; ++i) {
        w[i].th.id = i;
        w[i].step = (i % 4) + 1;
        w[i].count = 8;
    }
    HookLog h = {0};
    RunWorkers(w, MAX_WORKERS, 0, &h);
    int ok = 1;
    for (int i = 0; i < MAX_WORKERS; ++i) {
        for (int k = 0; k < 8; ++k) {
            ok = ok && w[i].seen[k] == w[i].step * (k + 1);
        }
    }
    CHECK(ok);
    int due[33] = {1};  // минуты, в которые кто-то просыпается, и минута 0
    for (int i = 0; i < MAX_WORKERS; ++i) {
        for (int k = 1; k <= 8; ++k) {
            due[(size_t)w[i].step * k] = 1;
        }
    }
    int expect = 0, chain = 1;
    for (int m = 0; m <= 32; ++m) {
        expect += due[m];
    }
    for (int i = 0; i + 1 < h.calls && i + 1 < MAX_HOOKS; ++i) {
        chain = chain && h.next[i] == h.now[i + 1] && due[h.now[i]];
    }
    CHECK(h.calls == expect);
    CHECK(chain);
    CHECK(h.notQuiet == 0);
}

static void TestHookSeesQuiescence(void) {
    Worker w[4];
    memset(w, 0, sizeof w);
    for (int i = 0; i < 4; ++i) {
        w[i].th.id = i;
        w[i].step = i + 1;
        w[i].count = 3;
    }
    HookLog h = {0};
    RunWorkers(w, 4, 0, &h);
    CHECK(h.calls > 0);
    CHECK(h.notQuiet == 0);
    CHECK(h.seen == 4);  // в последнем покое все четыре ещё зарегистрированы
}

static void RunDeterministic(int *log, int *len, int *maxBusy) {
    Worker w[3];
    memset(w, 0, sizeof w);
    _Atomic int busy = 0, maxB = 0;
    const int ids[3] = {2, 0, 1};  // порождаем не по порядку, order = id
    for (int i = 0; i < 3; ++i) {
        w[i].th.id = ids[i];
        w[i].th.order = ids[i];
        w[i].step = 1;
        w[i].count = 5;
        w[i].log = log;
        w[i].logLen = len;
        w[i].busy = &busy;
        w[i].maxBusy = &maxB;
    }
    HookLog h = {0};
    RunWorkers(w, 3, 1, &h);
    *maxBusy = atomic_load(&maxB);
}

static void TestDeterministicOrder(void) {
    int a[15] = {0}, b[15] = {0}, la = 0, lb = 0, busyA = 0, busyB = 0;
    RunDeterministic(a, &la, &busyA);
    RunDeterministic(b, &lb, &busyB);
    CHECK(la == 15 && lb == 15);
    int ordered = 1;
    for (int i = 0; i < 15; ++i) {
        ordered = ordered && a[i] == i % 3;
    }
    CHECK(ordered);
    CHECK(memcmp(a, b, sizeof a) == 0);
    CHECK(busyA == 1 && busyB == 1);
}

/// Монитор из двух потоков: A под m ждёт flag на c, B (если есть) будит его по сценарию mode.
typedef struct {
    SimThread a, b;
    Mutex m;
    Cond c, d;
    int flag;  // под m
    int mode;  // сценарий B
    SimStatus aStatus;
    int aSaw;    // минута, в которую A вышел из ожидания
    int aWaits;  // сколько раз вернулся SimWaitOn
    int aHeld;   // LockdepHeldCount() у A сразу после возврата SimWaitOn
    int wrongGrant, rightGrant;
} MonCtx;

/// Сценарии B: разбудить через 2 минуты; сначала SimGrant с чужим условием; 5 минут
/// CondBroadcast без SimGrant; потока B нет.
enum { B_GRANT_AFTER_2, B_WRONG_COND, B_SPURIOUS, B_NONE };

static const WaitReason queueWait = {.kind = WAIT_QUEUE, .stage = 0, .post = -1};

static void *MonWaiter(void *arg) {
    MonCtx *c = arg;
    if (SimBegin(&c->a) == SIM_OK) {
        MutexLock(&c->m);
        while (!c->flag && c->aStatus == SIM_OK) {
            c->aStatus = SimWaitOn(&c->a, &c->c, &c->m, queueWait);
            c->aHeld = LockdepHeldCount();
            ++c->aWaits;
        }
        c->aSaw = SimNow();
        MutexUnlock(&c->m);
    }
    SimExit(&c->a);
    return NULL;
}

/// То же, но во время ожидания A держит ещё один мьютекс (нарушение контракта SimWaitOn).
static void *ExtraLockWaiter(void *arg) {
    MonCtx *c = arg;
    if (SimBegin(&c->a) == SIM_OK) {
        Mutex extra;
        MutexInit(&extra, RANK_ENTRANCE, "extra test");
        MutexLock(&c->m);
        MutexLock(&extra);
        while (!c->flag && c->aStatus == SIM_OK) {
            c->aStatus = SimWaitOn(&c->a, &c->c, &c->m, queueWait);
            ++c->aWaits;
        }
        c->aSaw = SimNow();
        MutexUnlock(&extra);
        MutexUnlock(&c->m);
        MutexDestroy(&extra);
    }
    SimExit(&c->a);
    return NULL;
}

/// Под m: flag = 1, SimGrant(A, c), CondBroadcast(c).
static void GrantA(MonCtx *c) {
    MutexLock(&c->m);
    c->flag = 1;
    c->rightGrant = SimGrant(&c->a, &c->c);
    CondBroadcast(&c->c);
    MutexUnlock(&c->m);
}

static void *MonWaker(void *arg) {
    MonCtx *c = arg;
    if (SimBegin(&c->b) == SIM_OK) {
        if (c->mode == B_GRANT_AFTER_2) {
            if (SimSleep(&c->b, 2) == SIM_OK) {
                GrantA(c);
            }
        } else if (c->mode == B_WRONG_COND) {
            if (SimSleep(&c->b, 1) == SIM_OK) {
                MutexLock(&c->m);
                c->wrongGrant = SimGrant(&c->a, &c->d);
                CondBroadcast(&c->c);  // c будится, но разрешения выйти нет
                MutexUnlock(&c->m);
                if (SimSleep(&c->b, 1) == SIM_OK) {
                    GrantA(c);
                }
            }
        } else {
            int i = 0;
            for (; i < 5 && SimSleep(&c->b, 1) == SIM_OK; ++i) {
                MutexLock(&c->m);
                CondBroadcast(&c->c);
                MutexUnlock(&c->m);
            }
            if (i == 5) {
                GrantA(c);
            }
        }
    }
    SimExit(&c->b);
    return NULL;
}

static void RunMon(MonCtx *c, int deterministic, HookLog *h, void *(*waiter)(void *)) {
    SimOptions o = {.deterministic = deterministic, .delayMs = 0, .hook = RecordHook, .ctx = h};
    CHECK(SimInit(&o) == 0);
    MutexInit(&c->m, RANK_STAGE, "stage test");
    CHECK(CondInit(&c->c) == 0);
    CHECK(CondInit(&c->d) == 0);
    c->aSaw = -1;
    h->watch = &c->a;
    const int withB =
        c->mode != B_NONE;  // одной переменной: так clang-analyzer видит, что tb создан
    pthread_t ta, tb, clock;
    CHECK(SimThreadInit(&c->a, TH_CAR, 1, "car A", 0) == 0);
    SimSpawned(&c->a);
    CHECK(ThreadCreate(&ta, waiter, c, 0) == 0);
    if (withB) {
        CHECK(SimThreadInit(&c->b, TH_POST, 2, "post B", 1) == 0);
        SimSpawned(&c->b);
        CHECK(ThreadCreate(&tb, MonWaker, c, 0) == 0);
    }
    CHECK(ThreadCreate(&clock, SimClockMain, NULL, 0) == 0);
    pthread_join(clock, NULL);
    pthread_join(ta, NULL);
    SimThreadDestroy(&c->a);
    if (withB) {
        pthread_join(tb, NULL);
        SimThreadDestroy(&c->b);
    }
    CondDestroy(&c->c);
    CondDestroy(&c->d);
    MutexDestroy(&c->m);
}

/// SimGrant не двигает время: A выходит в ту минуту, в которую его разбудили.
static void TestGrantDoesNotAdvanceTime(void) {
    for (int det = 0; det <= 1; ++det) {
        MonCtx c = {.mode = B_GRANT_AFTER_2};
        HookLog h = {0};
        RunMon(&c, det, &h, MonWaiter);
        CHECK(c.rightGrant == 1);
        CHECK(c.aStatus == SIM_OK && c.aWaits == 1 && c.aHeld == 1);
        CHECK(c.aSaw == 2);
        CHECK(h.calls == 2);  // 0 (A ждёт, B спит), 2 (оба вышли) - и конец
        CHECK(h.now[0] == 0 && h.next[0] == 2);
        CHECK(h.now[1] == 2 && h.next[1] == -1);
        CHECK(h.watchState[0] == ST_PARKED);
        CHECK(h.notQuiet == 0);
        CHECK(SimTotalParks() == 1);
    }
}

/// SimGrant с чужим условием ничего не разрешает, даже если c потом будят.
static void TestGrantOnlyMatchingCond(void) {
    for (int det = 0; det <= 1; ++det) {
        MonCtx c = {.mode = B_WRONG_COND};
        HookLog h = {0};
        RunMon(&c, det, &h, MonWaiter);
        CHECK(c.wrongGrant == 0);
        CHECK(c.rightGrant == 1);
        CHECK(c.aWaits == 1 && c.aSaw == 2);
        CHECK(h.calls == 3);
        CHECK(h.now[1] == 1 && h.watchState[1] == ST_PARKED);
        CHECK(h.notQuiet == 0);
    }
}

/// broadcast без SimGrant не запускает A и не ломает счёт работающих.
static void TestSpuriousWakeIgnored(void) {
    for (int det = 0; det <= 1; ++det) {
        MonCtx c = {.mode = B_SPURIOUS};
        HookLog h = {0};
        RunMon(&c, det, &h, MonWaiter);
        CHECK(c.rightGrant == 1);
        CHECK(c.aWaits == 1);
        CHECK(c.aSaw == 5);
        CHECK(h.calls == 6);
        int parked = 1;
        for (int i = 0; i < 6 && i < h.calls; ++i) {
            parked = parked && h.now[i] == i && (i == 5 || h.watchState[i] == ST_PARKED);
        }
        CHECK(parked);
        CHECK(h.notQuiet == 0);
    }
}

/// Остановка модели будит ждущего на условной переменной: SIM_STOP с захваченным m.
static void TestStopWakesCondWaiters(void) {
    for (int det = 0; det <= 1; ++det) {
        MonCtx c = {.mode = B_NONE};
        HookLog h = {0};
        RunMon(&c, det, &h, MonWaiter);
        CHECK(c.aStatus == SIM_STOP);
        CHECK(c.aHeld == 1);
        CHECK(c.aWaits == 1);
        CHECK(h.calls == 1 && h.now[0] == 0 && h.next[0] == -1);
        CHECK(SimStopping());
    }
}

static void TestWaitOnWithExtraLock(void) {
    MonCtx c = {.mode = B_GRANT_AFTER_2};
    HookLog h = {0};
    long before = SoftAssertFailures();
    RunMon(&c, 0, &h, ExtraLockWaiter);
    CHECK(SoftAssertFailures() == before + 1);
    CHECK(c.aStatus == SIM_OK && c.aSaw == 2);
}

/// -D: три потока ждут на одном условии; разрешённые выходят строго в порядке SimGrant.
typedef struct GrantCtx GrantCtx;

typedef struct {
    SimThread th;
    GrantCtx *g;
    int idx;
    SimStatus st;
} GrantWaiter;

struct GrantCtx {
    SimThread b;
    GrantWaiter w[3];
    Mutex m;
    Cond c;
    int go[3];  // под m
    int log[3];
    int len;  // журнал выходов, без мьютекса: в -D работает один поток
};

static void *GrantWaiterMain(void *arg) {
    GrantWaiter *w = arg;
    GrantCtx *g = w->g;
    if (SimBegin(&w->th) == SIM_OK) {
        MutexLock(&g->m);
        while (!g->go[w->idx] && w->st == SIM_OK) {
            w->st = SimWaitOn(&w->th, &g->c, &g->m, queueWait);
        }
        MutexUnlock(&g->m);
        if (w->st == SIM_OK && g->len < 3) {
            g->log[g->len++] = w->idx;
        }
    }
    SimExit(&w->th);
    return NULL;
}

static void *GrantOrderMain(void *arg) {
    GrantCtx *g = arg;
    if (SimBegin(&g->b) == SIM_OK && SimSleep(&g->b, 1) == SIM_OK) {
        static const int order[3] = {2, 0, 1};
        MutexLock(&g->m);
        for (int k = 0; k < 3; ++k) {
            g->go[order[k]] = 1;
            SimGrant(&g->w[order[k]].th, &g->c);
        }
        CondBroadcast(&g->c);
        MutexUnlock(&g->m);
        // держатель жетона снова берёт m, пока разрешённые ждут жетона: они должны ждать без m
        SleepMs(20);
        MutexLock(&g->m);
        MutexUnlock(&g->m);
    }
    SimExit(&g->b);
    return NULL;
}

static void RunGrantOrder(GrantCtx *g) {
    HookLog h = {0};
    SimOptions o = {.deterministic = 1, .delayMs = 0, .hook = RecordHook, .ctx = &h};
    CHECK(SimInit(&o) == 0);
    MutexInit(&g->m, RANK_STAGE, "stage test");
    CHECK(CondInit(&g->c) == 0);
    pthread_t tw[3], tb, clock;
    for (int i = 0; i < 3; ++i) {
        g->w[i].g = g;
        g->w[i].idx = i;
        CHECK(SimThreadInit(&g->w[i].th, TH_CAR, i, "car", i) == 0);
        SimSpawned(&g->w[i].th);
        CHECK(ThreadCreate(&tw[i], GrantWaiterMain, &g->w[i], 0) == 0);
    }
    CHECK(SimThreadInit(&g->b, TH_POST, 3, "post B", 3) == 0);
    SimSpawned(&g->b);
    CHECK(ThreadCreate(&tb, GrantOrderMain, g, 0) == 0);
    CHECK(ThreadCreate(&clock, SimClockMain, NULL, 0) == 0);
    pthread_join(clock, NULL);
    for (int i = 0; i < 3; ++i) {
        pthread_join(tw[i], NULL);
        SimThreadDestroy(&g->w[i].th);
    }
    pthread_join(tb, NULL);
    SimThreadDestroy(&g->b);
    CondDestroy(&g->c);
    MutexDestroy(&g->m);
    CHECK(h.notQuiet == 0);
}

static void TestDeterministicGrantOrder(void) {
    static GrantCtx g1, g2;
    memset(&g1, 0, sizeof g1);
    memset(&g2, 0, sizeof g2);
    RunGrantOrder(&g1);
    RunGrantOrder(&g2);
    CHECK(g1.len == 3 && g2.len == 3);
    CHECK(g1.log[0] == 2 && g1.log[1] == 0 && g1.log[2] == 1);
    CHECK(memcmp(g1.log, g2.log, sizeof g1.log) == 0);
}

static int CountHook(int now, int nextWake, void *ctx) {
    (void)now;
    (void)nextWake;
    atomic_fetch_add((atomic_int *)ctx, 1);
    return 0;
}

/// С барьером старта часы не ищут покой, пока не пришли все участники.
static void TestClockWaitsStartBarrier(void) {
    Barrier b;
    atomic_int calls = 0;
    CHECK(BarrierInit(&b, 2) == 0);
    SimOptions o = {.hook = CountHook, .ctx = &calls, .start = &b};
    CHECK(SimInit(&o) == 0);
    pthread_t clock;
    CHECK(ThreadCreate(&clock, SimClockMain, NULL, 0) == 0);
    SleepMs(100);
    CHECK(atomic_load(&calls) == 0);
    BarrierWait(&b);
    pthread_join(clock, NULL);
    CHECK(atomic_load(&calls) == 1);
    BarrierDestroy(&b);
}

/// Крючок для SimKick: считает вызовы, о каждом сообщает через cv, на stopAt-м останавливает.
typedef struct {
    Mutex m;
    Cond cv;
    int calls, stopAt;  // под m
    int now[4];
} KickLog;

static int KickHook(int now, int nextWake, void *ctx) {
    (void)nextWake;
    KickLog *k = ctx;
    MutexLock(&k->m);
    if (k->calls < 4) {
        k->now[k->calls] = now;
    }
    int go = ++k->calls < k->stopAt;
    CondBroadcast(&k->cv);
    MutexUnlock(&k->m);
    return go;
}

/// Ждёт, пока крючок не будет вызван @p n раз.
static void WaitKickCalls(KickLog *k, int n) {
    MutexLock(&k->m);
    while (k->calls < n) {
        CondWait(&k->cv, &k->m);
    }
    MutexUnlock(&k->m);
}

static void KickLogInit(KickLog *k, int stopAt) {
    memset(k, 0, sizeof *k);
    MutexInit(&k->m, RANK_BUS, "kick test");
    CHECK(CondInit(&k->cv) == 0);
    k->stopAt = stopAt;
}

static void KickLogDestroy(KickLog *k) {
    CondDestroy(&k->cv);
    MutexDestroy(&k->m);
}

/// Часы ждут без спящих (nextWake == -1): SimKick заставляет их снова вызвать крючок.
static void TestKickRerunsHook(void) {
    KickLog k;
    KickLogInit(&k, 3);
    SimOptions o = {.delayMs = 0, .hook = KickHook, .ctx = &k};
    CHECK(SimInit(&o) == 0);
    pthread_t clock;
    CHECK(ThreadCreate(&clock, SimClockMain, NULL, 0) == 0);
    WaitKickCalls(&k, 1);  // первый покой: потоков нет, крючок вернул 1, часы ждут
    SimKick();
    WaitKickCalls(&k, 2);
    SimKick();  // третий вызов останавливает модель
    pthread_join(clock, NULL);
    CHECK(k.calls == 3);
    CHECK(k.now[0] == 0 && k.now[1] == 0 && k.now[2] == 0);
    KickLogDestroy(&k);
}

/// SimKick во время задержки темпа: крючок вызывается сразу, в ту же минуту, а не через -d.
static void TestKickCutsDelay(void) {
    KickLog k;
    KickLogInit(&k, 2);
    SimOptions o = {.delayMs = 5000, .hook = KickHook, .ctx = &k};
    CHECK(SimInit(&o) == 0);
    Worker w = {.th = {.id = 0}, .step = 1, .count = 8};
    pthread_t tid, clock;
    CHECK(SimThreadInit(&w.th, TH_POST, 0, "post", 0) == 0);
    SimSpawned(&w.th);
    CHECK(ThreadCreate(&tid, WorkerMain, &w, 0) == 0);
    CHECK(ThreadCreate(&clock, SimClockMain, NULL, 0) == 0);
    WaitKickCalls(&k, 1);  // крючок вернул 1, часы в задержке на 5 с
    long t0 = NowMs();
    SimKick();
    pthread_join(clock, NULL);
    CHECK(NowMs() - t0 < 1000);
    CHECK(k.calls == 2 && k.now[1] == k.now[0]);
    pthread_join(tid, NULL);
    CHECK(w.last == SIM_STOP);
    SimThreadDestroy(&w.th);
    KickLogDestroy(&k);
}

// ---- ThreadCreate ----

static void *Nop(void *arg) {
    return arg;
}

static void TestThreadFailHook(void) {
    pthread_t t;
    setenv("CARWASH_FAIL_THREAD_AFTER", "0", 1);  // NOLINT(concurrency-mt-unsafe)
    CHECK(ThreadCreate(&t, Nop, NULL, 0) == EAGAIN);
    unsetenv("CARWASH_FAIL_THREAD_AFTER");  // NOLINT(concurrency-mt-unsafe)
    // крючок перечитывается при каждом вызове
    CHECK(ThreadCreate(&t, Nop, NULL, CAR_STACK_SIZE) == 0);
    pthread_join(t, NULL);
}

int main(void) {
    RUN(TestStageCapacity);
    RUN(TestBadCapacity);
    RUN(TestRngStreams);
    RUN(TestSabotageNames);
    RUN(TestLockdepCatchesNesting);
    RUN(TestCondWaitKeepsLockdep);
    RUN(TestCondTimedWait);
    RUN(TestRwLockShared);
    RUN(TestRwLockExcludesWriter);
    RUN(TestRwLockLockdep);
    RUN(TestBarrierReleasesAll);
    RUN(TestBarrierReusable);
    RUN(TestThreadFailHook);
    RUN(TestSleepersWakeOnTime);
    RUN(TestManySleepersParallel);
    RUN(TestHookSeesQuiescence);
    RUN(TestDeterministicOrder);
    RUN(TestGrantDoesNotAdvanceTime);
    RUN(TestGrantOnlyMatchingCond);
    RUN(TestSpuriousWakeIgnored);
    RUN(TestStopWakesCondWaiters);
    RUN(TestWaitOnWithExtraLock);
    RUN(TestDeterministicGrantOrder);
    RUN(TestClockWaitsStartBarrier);
    RUN(TestKickRerunsHook);
    RUN(TestKickCutsDelay);
    return UnitSummary();
}
