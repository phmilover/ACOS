#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "auditor.h"
#include "barrier.h"
#include "config.h"
#include "events.h"
#include "gantt.h"
#include "line.h"
#include "memory.h"
#include "quiesce.h"
#include "render.h"
#include "rng.h"
#include "sim.h"
#include "snapshot.h"
#include "soft_assert.h"
#include "stage.h"
#include "sync.h"
#include "thread.h"
#include "unit.h"
#include "view_live.h"
#include "view_log.h"
#include "waitgraph.h"

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
    void (*onFirst)(void *ctx);      // что сделать в первом покое (NULL - ничего), затем продолжить
    void *firstCtx;
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
    if (h->calls == 1 && h->onFirst) {
        h->onFirst(h->firstCtx);
        return 1;
    }
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
    int aWoke;  // SleepAfterWaiter: минута конца сна после ожидания
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

/// После ожидания A спит 3 минуты.
static void *SleepAfterWaiter(void *arg) {
    MonCtx *c = arg;
    if (SimBegin(&c->a) == SIM_OK) {
        MutexLock(&c->m);
        while (!c->flag && c->aStatus == SIM_OK) {
            c->aStatus = SimWaitOn(&c->a, &c->c, &c->m, queueWait);
            ++c->aWaits;
        }
        c->aSaw = SimNow();
        MutexUnlock(&c->m);
        if (c->aStatus == SIM_OK && SimSleep(&c->a, 3) == SIM_OK) {
            c->aWoke = SimNow();
        }
    }
    SimExit(&c->a);
    return NULL;
}

static void GrantAFromHook(void *ctx) {
    GrantA(ctx);
    SleepMs(30);  // окно: если A запустился бы до конца крючка, он успел бы уснуть и проснуться
}

/// SimGrant из крючка (часы в это время никого не запускают): A запускается только после
/// крючка и считается работающим, поэтому его сон длится 3 минуты, а не кончается сразу.
static void TestGrantFromHook(void) {
    for (int det = 0; det <= 1; ++det) {
        MonCtx c = {.mode = B_NONE};
        HookLog h = {.onFirst = GrantAFromHook, .firstCtx = &c};
        RunMon(&c, det, &h, SleepAfterWaiter);
        CHECK(c.rightGrant == 1);
        CHECK(c.aStatus == SIM_OK && c.aWaits == 1 && c.aSaw == 0);
        CHECK(c.aWoke == 3);
        CHECK(h.calls == 3);  // 0 (A ждёт), 0 (A спит до 3), 3 (A вышел)
        CHECK(h.now[1] == 0 && h.next[1] == 3);
        CHECK(h.now[2] == 3 && h.next[2] == -1);
        CHECK(h.notQuiet == 0);
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
    int len;   // журнал выходов, без мьютекса: в -D работает один поток
    Mutex tm;  // TestStopWithTokenWaiters: B держит жетон, пока main не позовёт SimStop
    Cond tc;
    int bHolding, release;  // под tm
    SimStatus bAfter;
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

/// B разрешает выход потокам 1 и 2 (0 остаётся ждать на c) и держит жетон до release.
static void *StopHolderMain(void *arg) {
    GrantCtx *g = arg;
    if (SimBegin(&g->b) == SIM_OK && SimSleep(&g->b, 1) == SIM_OK) {
        MutexLock(&g->m);
        for (int k = 1; k < 3; ++k) {
            g->go[k] = 1;
            SimGrant(&g->w[k].th, &g->c);
        }
        CondBroadcast(&g->c);
        MutexUnlock(&g->m);
        MutexLock(&g->tm);
        g->bHolding = 1;
        CondBroadcast(&g->tc);
        while (!g->release) {
            CondWait(&g->tc, &g->tm);
        }
        MutexUnlock(&g->tm);
        g->bAfter = SimSleep(&g->b, 1);
    }
    SimExit(&g->b);
    return NULL;
}

/// [0] - разрешённые, вышедшие из ожидания на c и ждущие жетона; [1] - ждущие на c.
static void CountWaiters(const SimThread *t, void *ctx) {
    int *n = ctx;
    if (t->kind == TH_CAR && t->state == ST_READY && !t->waitCond) {
        ++n[0];
    } else if (t->kind == TH_CAR && t->state == ST_PARKED && t->waitCond) {
        ++n[1];
    }
}

/// main (не поток часов) зовёт SimStop, когда двое ждут жетона, а один - на условии.
static void StopAmidTokenWaiters(GrantCtx *g) {
    MutexLock(&g->tm);
    while (!g->bHolding) {
        CondWait(&g->tc, &g->tm);
    }
    MutexUnlock(&g->tm);
    int n[2] = {0, 0};
    for (int i = 0; i < 500 && (n[0] != 2 || n[1] != 1); ++i) {
        SleepMs(2);
        n[0] = n[1] = 0;
        SimForEach(CountWaiters, n);  // вне крючка - только чтобы подсмотреть состояние
    }
    CHECK(n[0] == 2 && n[1] == 1);
    SimStop();
    MutexLock(&g->tm);
    g->release = 1;
    CondBroadcast(&g->tc);
    MutexUnlock(&g->tm);
}

static void RunGrantOrder(GrantCtx *g, void *(*bMain)(void *), void (*during)(GrantCtx *g)) {
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
    CHECK(ThreadCreate(&tb, bMain, g, 0) == 0);
    CHECK(ThreadCreate(&clock, SimClockMain, NULL, 0) == 0);
    if (during) {
        during(g);
    }
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
    RunGrantOrder(&g1, GrantOrderMain, NULL);
    RunGrantOrder(&g2, GrantOrderMain, NULL);
    CHECK(g1.len == 3 && g2.len == 3);
    CHECK(g1.log[0] == 2 && g1.log[1] == 0 && g1.log[2] == 1);
    CHECK(memcmp(g1.log, g2.log, sizeof g1.log) == 0);
}

/// -D: SimStop из чужого потока будит и ждущих на условии монитора, и ждущих жетона.
static void TestStopWithTokenWaiters(void) {
    static GrantCtx g;
    memset(&g, 0, sizeof g);
    MutexInit(&g.tm, RANK_BUS, "holder test");
    CHECK(CondInit(&g.tc) == 0);
    RunGrantOrder(&g, StopHolderMain, StopAmidTokenWaiters);
    CHECK(g.w[0].st == SIM_STOP && g.w[1].st == SIM_STOP && g.w[2].st == SIM_STOP);
    CHECK(g.len == 0);
    CHECK(g.bAfter == SIM_STOP);
    CHECK(SimStopping());
    CondDestroy(&g.tc);
    MutexDestroy(&g.tm);
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

// ---- Шина событий и лента ----

/// Конфигурация normal (встроенная): prewash, tunnel, wax, dry.
static void LoadNormal(Config *c) {
    char err[256];
    ConfigDefaults(c);
    CHECK(ConfigParseText(c, CONFIG_BUILTIN, err, sizeof err) == 0);
}

static void StartBusSim(void) {
    SimOptions o = {0};
    CHECK(SimInit(&o) == 0);
}

#define BUS_THREADS 8
#define BUS_EMITS   1000

static long busLastSeq, busCount, busViolations;  // трогает только слушатель, под мьютексом шины

static void CountingListener(const Event *e) {
    if (e->seq != busLastSeq + 1) {
        ++busViolations;
    }
    busLastSeq = e->seq;
    ++busCount;
}

static void *EmitterMain(void *arg) {
    for (int i = 0; i < BUS_EMITS; ++i) {
        EV(EV_ARRIVE, *(const int *)arg, -1, -1, -1, 0);
    }
    return NULL;
}

/// 8 потоков × 1000 событий: номера идут строго подряд, ничего не потеряно.
static void TestConcurrentEmitOrdered(void) {
    Config c;
    LoadNormal(&c);
    StartBusSim();
    EventsInit(&c);
    busLastSeq = -1;
    busCount = busViolations = 0;
    EventsSubscribe(CountingListener);
    pthread_t th[BUS_THREADS];
    int ids[BUS_THREADS];
    for (int i = 0; i < BUS_THREADS; ++i) {
        ids[i] = i;
        CHECK(ThreadCreate(&th[i], EmitterMain, &ids[i], 0) == 0);
    }
    for (int i = 0; i < BUS_THREADS; ++i) {
        pthread_join(th[i], NULL);
    }
    CHECK(busCount == (long)BUS_THREADS * BUS_EMITS);
    CHECK(busViolations == 0);
}

static void TestDescribeTexts(void) {
    Config c;
    char buf[200];
    LoadNormal(&c);
    EventsInit(&c);
    Event move = {.type = EV_MOVE, .car = 6, .stage = 1, .post = -1, .program = 2, .arg = 0};
    CHECK(EventDescribe(&move, buf, sizeof buf) > 0);
    CHECK(strcmp(buf, "car #7 moves prewash -> tunnel") == 0);
    Event fail = {.type = EV_FAIL, .car = 2, .stage = 1, .post = 0, .program = 1, .arg = 9};
    EventDescribe(&fail, buf, sizeof buf);
    CHECK(strstr(buf, "BREAKS DOWN with car #3") != NULL);
    fail.car = -1;
    EventDescribe(&fail, buf, sizeof buf);
    CHECK(strstr(buf, "BREAKS DOWN (repair 9 min)") != NULL);
    Event done = {.type = EV_DONE, .car = 0, .stage = -1, .post = -1, .program = 2, .arg = 30};
    EventDescribe(&done, buf, sizeof buf);
    CHECK(strstr(buf, "is clean!") != NULL);
    CHECK(strstr(buf, "Premium done in 30 min") != NULL);  // программа берётся из события
    Event prog = {.type = EV_PROGRAM, .car = 0, .stage = -1, .post = -1, .program = 2, .arg = 2};
    EventDescribe(&prog, buf, sizeof buf);
    CHECK(strcmp(buf, "car #1 chooses Premium (prewash > tunnel > wax > dry)") == 0);
    char clock[16];
    EventClock(17, clock, sizeof clock);
    CHECK(strcmp(clock, "08:17") == 0);
    EventClock((24 * 60) + 17, clock, sizeof clock);
    CHECK(strcmp(clock, "d2 08:17") == 0);
}

static char srcSeen[40];  // слушатель пишет, тест читает после Emit (в том же потоке)

static void SourceListener(const Event *e) {
    EventSource(e, srcSeen, sizeof srcSeen);
}

static void TestSourceNames(void) {
    Config c;
    LoadNormal(&c);
    StartBusSim();
    EventsInit(&c);
    EventsSubscribe(SourceListener);
    SimThread car = {.kind = TH_CAR, .id = 6};
    SimThread post = {.kind = TH_POST, .id = (1 * MAX_POSTS) + 1};  // tunnel, второй пост
    SimThread coord = {.kind = TH_COORD, .id = 0};
    EventsSetSource(&car);
    EV(EV_ARRIVE, 6, -1, -1, -1, 0);
    CHECK(strcmp(srcSeen, "car #7") == 0);
    EventsSetSource(&post);
    EV(EV_ENTER, 6, 1, 1, 0, 0);
    CHECK(strcmp(srcSeen, "post tunnel.2") == 0);
    EventsSetSource(&coord);
    EV(EV_CLOSE, -1, -1, -1, -1, 0);
    CHECK(strcmp(srcSeen, "coordinator") == 0);
    EventsSetSource(NULL);  // главный поток, не звавший EventsSetSource, - то же самое
    EV(EV_CLOSE, -1, -1, -1, -1, 0);
    CHECK(strcmp(srcSeen, "main") == 0);
}

/// Лента: формат строки без колонки и с колонкой источника.
static void TestViewLogFormat(void) {
    Config c;
    LoadNormal(&c);
    StartBusSim();
    int fd[2];
    CHECK(pipe(fd) == 0);
    SimThread car = {.kind = TH_CAR, .id = 6};
    for (int column = 0; column <= 1; ++column) {
        EventsInit(&c);
        ViewLogInit(fd[1], 0, -1, column);
        EventsSubscribe(ViewLogListener);
        EventsSetSource(&car);
        EV(EV_MOVE, 6, 1, -1, 2, 0);
        char buf[200] = "";
        ssize_t n = read(fd[0], buf, sizeof buf - 1);
        CHECK(n > 0);
        CHECK(strcmp(buf, column ? "[08:00] car #7         | car #7 moves prewash -> tunnel\n"
                                 : "[08:00] car #7 moves prewash -> tunnel\n") == 0);
    }
    EventsSetSource(NULL);
    close(fd[0]);
    close(fd[1]);
}

// ---- Монитор стадии: однопоточно (машины - записи без потоков, предикаты зовутся напрямую) ----

/// Как data/capacity.cfg: один пост сушки на две машины.
static const char *CAPACITY_CFG = "stage = prewash 1 3 5 3\nstage = tunnel 2 6 9 2\n"
                                  "stage = wax 1 4 6 2\nstage = dry 1 3 5 2 2\n"
                                  "program = Express 40 1 tunnel dry\n"
                                  "program = Standard 40 2 prewash tunnel dry\n"
                                  "program = Premium 20 3 prewash tunnel wax dry\n";
/// Как data/zero-buffer.cfg: накопителей нет.
static const char *ZERO_CFG = "stage = prewash 1 3 5 0\nstage = tunnel 2 6 9 0\n"
                              "stage = wax 1 4 6 0\nstage = dry 1 3 5 0\n"
                              "program = Express 40 1 tunnel dry\n"
                              "program = Standard 40 2 prewash tunnel dry\n"
                              "program = Premium 20 3 prewash tunnel wax dry\n";
/// Как data/priority.cfg: один пост тоннеля, очередь 4.
static const char *PRIORITY_CFG = "stage = prewash 1 3 5 4\nstage = tunnel 1 6 8 4\n"
                                  "stage = wax 1 4 6 2\nstage = dry 1 3 5 4\n"
                                  "program = Express 40 1 tunnel dry\n"
                                  "program = Standard 40 2 prewash tunnel dry\n"
                                  "program = Premium 20 3 prewash tunnel wax dry\n"
                                  "priority = program\n";
/// Одна стадия: один пост, очередь на одну машину.
static const char *ONE_CFG = "stage = a 1 3 5 1\nprogram = P 1 1 a\n";
/// Как data/dead-stage.cfg: у wax нет постов.
static const char *DEAD_CFG = "stage = prewash 1 3 5 3\nstage = tunnel 2 6 9 2\n"
                              "stage = wax 0 4 6 2\nstage = dry 2 3 5 3\n"
                              "program = Express 40 1 tunnel dry\n"
                              "program = Standard 40 2 prewash tunnel dry\n"
                              "program = Premium 20 3 prewash tunnel wax dry\n";

enum { P_EXPRESS = 0, P_STANDARD = 1, P_PREMIUM = 2 };  // программы normal и производных
enum { S_PREWASH = 0, S_TUNNEL = 1, S_WAX = 2, S_DRY = 3 };

#define EV_LOG_MAX 256
static Event evLog[EV_LOG_MAX];  // слушатель пишет под мьютексом шины, тест читает после join
static int evCount;

static void RecordListener(const Event *e) {
    if (evCount < EV_LOG_MAX) {
        evLog[evCount++] = *e;
    }
}

/// @return номер первого события type машины car в журнале теста или -1
static int FindEv(EventType type, int car) {
    for (int i = 0; i < evCount; ++i) {
        if (evLog[i].type == type && evLog[i].car == car) {
            return i;
        }
    }
    return -1;
}

static Config lineCfg;  // линия большая - держим статически
static Line line;

/// Конфигурация из текста (NULL - встроенная normal).
static Config *LoadLineCfg(const char *text) {
    char err[256];
    ConfigDefaults(&lineCfg);
    CHECK(ConfigParseText(&lineCfg, text ? text : CONFIG_BUILTIN, err, sizeof err) == 0);
    CHECK(ConfigValidate(&lineCfg, err, sizeof err) == 0);
    return &lineCfg;
}

/// Свежая модель с параметрами @p o, шина с записью событий и линия по lineCfg.
static Line *OpenLineWith(const SimOptions *o) {
    CHECK(SimInit(o) == 0);
    EventsInit(&lineCfg);
    EventsSubscribe(RecordListener);
    evCount = 0;
    CHECK(LineInit(&line, &lineCfg) == 0);
    return &line;
}

static Line *OpenLine(void) {
    SimOptions o = {0};
    return OpenLineWith(&o);
}

/// Машина без потока (только SimThread), в реестре линии; LineFree её освободит.
static Car *NewCar(Line *l, int program) {
    Car *car = calloc(1, sizeof *car);
    if (car == NULL) {
        abort();
    }
    RwLockWrite(&l->registry);
    if (l->carCount == l->carCap) {
        l->carCap = l->carCap ? l->carCap * 2 : 8;
        l->cars = (Car **)CheckedRealloc((void *)l->cars, (size_t)l->carCap * sizeof *l->cars);
    }
    car->id = l->carCount;
    l->cars[l->carCount++] = car;
    RwLockUnlock(&l->registry);
    char name[24];
    snprintf(name, sizeof name, "car #%d", car->id + 1);
    CHECK(SimThreadInit(&car->th, TH_CAR, car->id, name, 0) == 0);
    car->line = l;
    car->program = program;
    car->place = CAR_ARRIVING;
    car->stage = car->post = car->seat = -1;
    car->res = RES_NONE;
    car->resStage = car->resPost = car->resSeat = -1;
    car->ticket = -1;
    car->finished = -1;
    return car;
}

/// StageEnter на стадии s.
static Admit Enter(Line *l, int s, Car *car) {
    Stage *st = &l->stage[s];
    MutexLock(&st->m);
    Admit a = StageEnter(l, st, car);
    MutexUnlock(&st->m);
    return a;
}

/// StageReserve на стадии s, когда место заведомо есть (иначе без потока машины ждать нельзя).
/// @return полученная бронь
static Reservation Reserve(Line *l, int s, Car *car) {
    Stage *st = &l->stage[s];
    MutexLock(&st->m);
    CHECK(StageRoomTurn(l, st, car) != RES_NONE || car->res != RES_NONE);
    CHECK(StageReserve(l, st, car, S_PREWASH, 0) == SIM_OK);
    Reservation r = car->res;
    MutexUnlock(&st->m);
    return r;
}

static Reservation RoomTurn(Line *l, int s, const Car *car) {
    Stage *st = &l->stage[s];
    MutexLock(&st->m);
    Reservation r = StageRoomTurn(l, st, car);
    MutexUnlock(&st->m);
    return r;
}

static void Commit(Line *l, int s, Car *car, int from) {
    Stage *st = &l->stage[s];
    MutexLock(&st->m);
    StageCommit(l, st, car, from);
    MutexUnlock(&st->m);
}

/// @return 1, если очередь car садиться (место в *post)
static int QueueTurn(Line *l, int s, const Car *car, int *post) {
    Stage *st = &l->stage[s];
    int seat = -1;
    MutexLock(&st->m);
    int turn = StageQueueTurn(l, st, car, post, &seat);
    MutexUnlock(&st->m);
    return turn;
}

/// Машина, чья очередь уже наступила, садится (StageAwaitSeat без ожидания).
static void TakeSeat(Line *l, int s, Car *car) {
    Stage *st = &l->stage[s];
    MutexLock(&st->m);
    CHECK(StageAwaitSeat(l, st, car) == SIM_OK);
    MutexUnlock(&st->m);
}

/// Операция машины на стадии s закончилась (как у поста), и машина уезжает.
static void Vacate(Line *l, int s, Car *car) {
    Stage *st = &l->stage[s];
    MutexLock(&st->m);
    if (car->place == CAR_POST && car->stage == s) {
        st->post[car->post].seat[car->seat].left = 0;
    }
    StageVacate(l, st, car);
    MutexUnlock(&st->m);
}

static int Queued(Line *l, int s) {
    MutexLock(&l->stage[s].m);
    int n = l->stage[s].queued;
    MutexUnlock(&l->stage[s].m);
    return n;
}

static int Waiters(Line *l, int s) {
    MutexLock(&l->stage[s].m);
    int n = l->stage[s].waiters;
    MutexUnlock(&l->stage[s].m);
    return n;
}

static void SetRepair(Line *l, int s, int p, int repair) {
    MutexLock(&l->stage[s].m);
    l->stage[s].post[p].repair = repair;
    MutexUnlock(&l->stage[s].m);
}

/// Ставит машину в список ожидающих стадии s с номером ticket (как StageReserve, но без
/// ожидания: потока у машины нет).
static void ListWaiter(Line *l, int s, Car *car, long ticket) {
    Stage *st = &l->stage[s];
    MutexLock(&st->m);
    st->waiting[st->waiters++] = car;
    car->ticket = ticket;
    MutexUnlock(&st->m);
}

static void TestAdmitDirectSeat(void) {
    LoadLineCfg(NULL);
    Line *l = OpenLine();
    Car *car = NewCar(l, P_EXPRESS);
    Stage *st = &l->stage[S_TUNNEL];
    CHECK(Enter(l, S_TUNNEL, car) == ADMIT_SEAT);
    MutexLock(&st->m);
    CHECK(car->res == RES_NONE && st->post[0].reserved == 0);
    CHECK(car->place == CAR_POST && car->stage == S_TUNNEL && car->post == 0 && car->seat == 0);
    int left = st->post[0].seat[0].left;
    CHECK(st->post[0].seat[0].car == car->id && left >= 6 && left <= 9);
    MutexUnlock(&st->m);
    int enter = FindEv(EV_ENTER, car->id), start = FindEv(EV_OP_START, car->id);
    CHECK(enter >= 0 && start == enter + 1);
    CHECK(start >= 0 && evLog[start].arg == left && evLog[start].program == P_EXPRESS);
    CHECK(car->service == left);
    CHECK(FindEv(EV_MOVE, car->id) == -1);  // с въезда - без переезда
    LineFree(l);
}

static void TestAdmitQueueThenFull(void) {
    LoadLineCfg(NULL);  // tunnel: 2 поста по одному месту, очередь 2
    Line *l = OpenLine();
    Admit want[5] = {ADMIT_SEAT, ADMIT_SEAT, ADMIT_QUEUE, ADMIT_QUEUE, ADMIT_FULL};
    Car *cars[5];
    for (int i = 0; i < 5; ++i) {
        cars[i] = NewCar(l, P_EXPRESS);
        CHECK(Enter(l, S_TUNNEL, cars[i]) == want[i]);
    }
    CHECK(cars[0]->post == 0 && cars[1]->post == 1);
    CHECK(cars[2]->place == CAR_QUEUE && cars[3]->place == CAR_QUEUE);
    CHECK(cars[4]->place == CAR_ARRIVING && cars[4]->ticket == -1 && cars[4]->res == RES_NONE);
    CHECK(Queued(l, S_TUNNEL) == 2 && Waiters(l, S_TUNNEL) == 0);
    int q = FindEv(EV_QUEUE, cars[3]->id);
    CHECK(q >= 0 && evLog[q].arg == 2);
    CHECK(FindEv(EV_QUEUE, cars[4]->id) == -1 && FindEv(EV_ENTER, cars[4]->id) == -1);
    LineFree(l);
}

static void TestCapacityTwo(void) {
    LoadLineCfg(CAPACITY_CFG);
    Line *l = OpenLine();
    Car *a = NewCar(l, P_EXPRESS), *b = NewCar(l, P_EXPRESS), *c = NewCar(l, P_EXPRESS);
    CHECK(Enter(l, S_DRY, a) == ADMIT_SEAT);
    CHECK(Enter(l, S_DRY, b) == ADMIT_SEAT);
    CHECK(Enter(l, S_DRY, c) == ADMIT_QUEUE);
    CHECK(a->post == 0 && b->post == 0 && a->seat == 0 && b->seat == 1);
    CHECK(c->place == CAR_QUEUE);
    LineFree(l);
}

static void TestReservationCounts(void) {
    LoadLineCfg(ONE_CFG);
    Line *l = OpenLine();
    Car *a = NewCar(l, 0), *b = NewCar(l, 0), *c = NewCar(l, 0);
    Stage *st = &l->stage[0];
    CHECK(Reserve(l, 0, a) == RES_SEAT);  // бронь места, без Commit
    MutexLock(&st->m);
    CHECK(a->resStage == 0 && a->resPost == 0 && st->post[0].reserved == 1);
    MutexUnlock(&st->m);
    CHECK(Reserve(l, 0, b) == RES_QUEUE);  // место занято бронью
    MutexLock(&st->m);
    CHECK(st->queueReserved == 1 && st->queued == 0);
    MutexUnlock(&st->m);
    CHECK(RoomTurn(l, 0, c) == RES_NONE);  // бронь очереди учтена во вместимости
    CHECK(Enter(l, 0, c) == ADMIT_FULL);
    CHECK(Reserve(l, 0, a) == RES_SEAT);  // повторный вызов возвращает ту же бронь
    MutexLock(&st->m);
    CHECK(st->post[0].reserved == 1);
    MutexUnlock(&st->m);
    Commit(l, 0, a, -1);
    Commit(l, 0, b, -1);
    MutexLock(&st->m);
    CHECK(st->queueReserved == 0 && st->queued == 1 && b->place == CAR_QUEUE);
    CHECK(st->post[0].reserved == 0 && a->place == CAR_POST);
    MutexUnlock(&st->m);
    CHECK(RoomTurn(l, 0, c) == RES_NONE);
    LineFree(l);

    // место освободилось, пока машина с бронью очереди ещё в пути: новая машина его не займёт
    LoadLineCfg(NULL);
    l = OpenLine();
    Car *t1 = NewCar(l, P_EXPRESS), *t2 = NewCar(l, P_EXPRESS);
    Car *q = NewCar(l, P_EXPRESS), *n = NewCar(l, P_EXPRESS);
    Enter(l, S_TUNNEL, t1);
    Enter(l, S_TUNNEL, t2);
    CHECK(Reserve(l, S_TUNNEL, q) == RES_QUEUE);  // бронь очереди, без Commit
    Vacate(l, S_TUNNEL, t1);
    CHECK(RoomTurn(l, S_TUNNEL, n) == RES_QUEUE);  // не SEAT: в очередь уже едет q
    Commit(l, S_TUNNEL, q, S_PREWASH);             // q въехал в очередь: свободное место - его
    CHECK(q->place == CAR_QUEUE);
    int post = -1;
    CHECK(QueueTurn(l, S_TUNNEL, q, &post) == 1 && post == 0);
    CHECK(Enter(l, S_TUNNEL, n) == ADMIT_QUEUE);
    CHECK(QueueTurn(l, S_TUNNEL, n, &post) == 0);  // fifo: q встал раньше
    TakeSeat(l, S_TUNNEL, q);
    CHECK(q->place == CAR_POST && q->post == 0 && n->place == CAR_QUEUE);
    int move = FindEv(EV_MOVE, q->id), qq = FindEv(EV_QUEUE, q->id);
    CHECK(move >= 0 && qq == move + 1 && FindEv(EV_ENTER, q->id) > qq);
    LineFree(l);
}

static void TestPriorityPop(void) {
    for (int fifo = 0; fifo <= 1; ++fifo) {
        Config *cfg = LoadLineCfg(PRIORITY_CFG);
        cfg->rule = fifo ? PRIO_FIFO : PRIO_PROGRAM;
        Line *l = OpenLine();
        Car *a = NewCar(l, P_STANDARD), *express = NewCar(l, P_EXPRESS);
        Car *premium = NewCar(l, P_PREMIUM);
        CHECK(Enter(l, S_TUNNEL, a) == ADMIT_SEAT);
        CHECK(Enter(l, S_TUNNEL, express) == ADMIT_QUEUE);
        CHECK(Enter(l, S_TUNNEL, premium) == ADMIT_QUEUE);
        int post = -1;
        CHECK(QueueTurn(l, S_TUNNEL, express, &post) == 0);  // места нет ни у кого
        CHECK(QueueTurn(l, S_TUNNEL, premium, &post) == 0);
        Vacate(l, S_TUNNEL, a);
        Car *won = fifo ? express : premium, *lost = fifo ? premium : express;
        CHECK(QueueTurn(l, S_TUNNEL, won, &post) == 1 && post == 0);
        CHECK(QueueTurn(l, S_TUNNEL, lost, &post) == 0);
        TakeSeat(l, S_TUNNEL, won);
        CHECK(won->place == CAR_POST && won->post == 0 && lost->place == CAR_QUEUE);
        CHECK(Queued(l, S_TUNNEL) == 1);
        int leave = FindEv(EV_LEAVE, a->id), enter = FindEv(EV_ENTER, won->id);
        CHECK(leave >= 0 && enter > leave && FindEv(EV_OP_START, won->id) == enter + 1);
        LineFree(l);
    }
}

/// Тоннель zero-buffer занят двумя машинами, ждущие места - по порядку номеров в списке.
static void TestWaitersOrder(void) {
    // ticket 4 (Standard) раньше ticket 5 (Express), хотя в списке Express стоит первым
    Config *cfg = LoadLineCfg(ZERO_CFG);
    cfg->rule = PRIO_FIFO;
    Line *l = OpenLine();
    Car *t1 = NewCar(l, P_EXPRESS), *t2 = NewCar(l, P_EXPRESS);
    CHECK(Enter(l, S_TUNNEL, t1) == ADMIT_SEAT && Enter(l, S_TUNNEL, t2) == ADMIT_SEAT);
    Car *standard = NewCar(l, P_STANDARD), *express = NewCar(l, P_EXPRESS);
    CHECK(RoomTurn(l, S_TUNNEL, express) == RES_NONE);  // мест нет
    ListWaiter(l, S_TUNNEL, express, 5);
    ListWaiter(l, S_TUNNEL, standard, 4);
    CHECK(RoomTurn(l, S_TUNNEL, standard) == RES_NONE);
    Vacate(l, S_TUNNEL, t1);
    CHECK(RoomTurn(l, S_TUNNEL, standard) == RES_SEAT);
    CHECK(RoomTurn(l, S_TUNNEL, express) == RES_NONE);
    Car *fresh = NewCar(l, P_EXPRESS);
    CHECK(RoomTurn(l, S_TUNNEL, fresh) == RES_NONE);  // не из списка: ожидающие есть
    CHECK(Enter(l, S_TUNNEL, fresh) == ADMIT_FULL);
    CHECK(Reserve(l, S_TUNNEL, standard) == RES_SEAT);
    CHECK(standard->ticket == -1 && standard->resPost == 0);
    CHECK(express->res == RES_NONE && express->ticket == 5 && Waiters(l, S_TUNNEL) == 1);
    CHECK(RoomTurn(l, S_TUNNEL, express) == RES_NONE);  // место ушло standard
    LineFree(l);

    // номер в списке важнее приоритета: Express (раньше) получает место раньше Premium
    cfg = LoadLineCfg(ZERO_CFG);
    cfg->rule = PRIO_PROGRAM;
    l = OpenLine();
    t1 = NewCar(l, P_EXPRESS);
    t2 = NewCar(l, P_EXPRESS);
    Enter(l, S_TUNNEL, t1);
    Enter(l, S_TUNNEL, t2);
    express = NewCar(l, P_EXPRESS);
    Car *premium = NewCar(l, P_PREMIUM);
    ListWaiter(l, S_TUNNEL, express, 1);
    ListWaiter(l, S_TUNNEL, premium, 2);
    Vacate(l, S_TUNNEL, t1);
    CHECK(RoomTurn(l, S_TUNNEL, express) == RES_SEAT);
    CHECK(RoomTurn(l, S_TUNNEL, premium) == RES_NONE);
    LineFree(l);

    // равные номера: выше приоритет, затем меньший номер машины
    cfg = LoadLineCfg(ZERO_CFG);
    cfg->rule = PRIO_PROGRAM;
    l = OpenLine();
    t1 = NewCar(l, P_EXPRESS);
    t2 = NewCar(l, P_EXPRESS);
    Enter(l, S_TUNNEL, t1);
    Enter(l, S_TUNNEL, t2);
    express = NewCar(l, P_EXPRESS);
    Car *p1 = NewCar(l, P_PREMIUM), *p2 = NewCar(l, P_PREMIUM);
    ListWaiter(l, S_TUNNEL, p2, 7);  // в список в обратном порядке
    ListWaiter(l, S_TUNNEL, p1, 7);
    ListWaiter(l, S_TUNNEL, express, 7);
    Vacate(l, S_TUNNEL, t1);
    CHECK(RoomTurn(l, S_TUNNEL, p1) == RES_SEAT);
    CHECK(RoomTurn(l, S_TUNNEL, p2) == RES_NONE && RoomTurn(l, S_TUNNEL, express) == RES_NONE);
    CHECK(Reserve(l, S_TUNNEL, p1) == RES_SEAT);
    Vacate(l, S_TUNNEL, t2);
    CHECK(RoomTurn(l, S_TUNNEL, p2) == RES_SEAT && RoomTurn(l, S_TUNNEL, express) == RES_NONE);
    CHECK(Reserve(l, S_TUNNEL, p2) == RES_SEAT);
    CHECK(express->res == RES_NONE && Waiters(l, S_TUNNEL) == 1);
    LineFree(l);
}

static void TestBrokenPostTakesNobody(void) {
    LoadLineCfg(NULL);
    Line *l = OpenLine();
    SetRepair(l, S_TUNNEL, 0, 5);
    Car *a = NewCar(l, P_EXPRESS), *b = NewCar(l, P_EXPRESS);
    CHECK(Reserve(l, S_TUNNEL, a) == RES_SEAT);
    CHECK(a->resPost == 1);  // не на сломанный пост 0
    CHECK(Reserve(l, S_TUNNEL, b) == RES_QUEUE);
    Commit(l, S_TUNNEL, a, -1);
    Commit(l, S_TUNNEL, b, -1);
    CHECK(b->place == CAR_QUEUE);
    int post = -1;
    CHECK(QueueTurn(l, S_TUNNEL, b, &post) == 0);  // на сломанный пост не садится
    CHECK(FindEv(EV_ENTER, b->id) == -1);
    Stage *st = &l->stage[S_TUNNEL];
    MutexLock(&st->m);
    st->post[0].repair = 0;  // починили
    StageRepaired(l, st);
    MutexUnlock(&st->m);
    CHECK(QueueTurn(l, S_TUNNEL, b, &post) == 1 && post == 0);
    TakeSeat(l, S_TUNNEL, b);
    CHECK(b->place == CAR_POST && b->post == 0 && Queued(l, S_TUNNEL) == 0);
    LineFree(l);
}

static void TestZeroQueue(void) {
    LoadLineCfg(ZERO_CFG);
    Line *l = OpenLine();
    Car *a = NewCar(l, P_EXPRESS), *b = NewCar(l, P_EXPRESS), *c = NewCar(l, P_EXPRESS);
    CHECK(Enter(l, S_TUNNEL, a) == ADMIT_SEAT);
    CHECK(Enter(l, S_TUNNEL, b) == ADMIT_SEAT);
    CHECK(Enter(l, S_TUNNEL, c) == ADMIT_FULL && Waiters(l, S_TUNNEL) == 0);
    CHECK(RoomTurn(l, S_TUNNEL, c) == RES_NONE);
    ListWaiter(l, S_TUNNEL, c, 1);
    Vacate(l, S_TUNNEL, b);
    CHECK(FindEv(EV_LEAVE, b->id) >= 0);
    CHECK(RoomTurn(l, S_TUNNEL, c) == RES_SEAT);
    CHECK(Reserve(l, S_TUNNEL, c) == RES_SEAT);
    CHECK(c->resPost == 1 && c->ticket == -1 && Waiters(l, S_TUNNEL) == 0);
    Commit(l, S_TUNNEL, c, S_PREWASH);
    int move = FindEv(EV_MOVE, c->id);
    CHECK(move >= 0 && evLog[move].stage == S_TUNNEL && evLog[move].arg == S_PREWASH);
    CHECK(FindEv(EV_ENTER, c->id) == move + 1 && c->post == 1);
    LineFree(l);

    // место появилось без пробуждения (пост починился, StageRepaired ещё не звали): новая
    // машина не обгоняет ожидающую
    LoadLineCfg(ZERO_CFG);
    l = OpenLine();
    SetRepair(l, S_TUNNEL, 0, 5);
    Car *t = NewCar(l, P_EXPRESS), *w = NewCar(l, P_EXPRESS), *n = NewCar(l, P_EXPRESS);
    CHECK(Enter(l, S_TUNNEL, t) == ADMIT_SEAT && t->post == 1);
    ListWaiter(l, S_TUNNEL, w, 1);
    SetRepair(l, S_TUNNEL, 0, 0);
    CHECK(Enter(l, S_TUNNEL, n) == ADMIT_FULL);
    CHECK(RoomTurn(l, S_TUNNEL, n) == RES_NONE);
    CHECK(Reserve(l, S_TUNNEL, w) == RES_SEAT && w->resPost == 0);
    LineFree(l);
}

static void TestSabotageOverflow(void) {
    Config *cfg = LoadLineCfg(ONE_CFG);
    cfg->sabotage = SAB_OVERFLOW;
    Line *l = OpenLine();
    Car *a = NewCar(l, 0), *b = NewCar(l, 0), *c = NewCar(l, 0), *d = NewCar(l, 0);
    CHECK(Enter(l, 0, a) == ADMIT_SEAT);
    CHECK(Enter(l, 0, b) == ADMIT_QUEUE);
    long before = SoftAssertFailures();
    CHECK(Enter(l, 0, c) == ADMIT_QUEUE);  // ошибка на единицу пускает лишнюю
    CHECK(SoftAssertFailures() > before);  // а Commit это замечает
    CHECK(Queued(l, 0) == 2);
    CHECK(Enter(l, 0, d) == ADMIT_FULL);
    LineFree(l);
}

/// Машина помылась на prewash и переезжает в тоннель через StageMove (как поток машины).
static void RunDeadlockScenario(Sabotage sab, long *violations) {
    Config *cfg = LoadLineCfg(ZERO_CFG);
    cfg->sabotage = sab;
    Line *l = OpenLine();
    Car *p = NewCar(l, P_STANDARD);
    CHECK(Enter(l, S_PREWASH, p) == ADMIT_SEAT);
    CHECK(Reserve(l, S_TUNNEL, p) == RES_SEAT);  // бронь в тоннеле
    MutexLock(&l->stage[S_PREWASH].m);
    l->stage[S_PREWASH].post[p->post].seat[p->seat].left = 0;  // операция кончилась
    MutexUnlock(&l->stage[S_PREWASH].m);
    long before = LockdepViolations();
    StageMove(l, &l->stage[S_PREWASH], &l->stage[S_TUNNEL], p);
    *violations = LockdepViolations() - before;
    CHECK(p->place == CAR_POST && p->stage == S_TUNNEL);  // переезд состоялся в обоих случаях
    CHECK(FindEv(EV_LEAVE, p->id) >= 0 && FindEv(EV_MOVE, p->id) > FindEv(EV_LEAVE, p->id));
    CHECK(LockdepHeldCount() == 0);
    LineFree(l);
}

/// SAB_DEADLOCK: первый же переезд берёт мьютекс следующей стадии, не отпустив текущую.
static void TestSabotageDeadlock(void) {
    LockdepSetFatal(0);
    long clean = -1, caught = -1;
    RunDeadlockScenario(SAB_NONE, &clean);
    RunDeadlockScenario(SAB_DEADLOCK, &caught);
    CHECK(clean == 0);
    CHECK(caught == 1);
    LockdepSetFatal(1);
}

/// SAB_OVERFLOW на стадии с двумя постами: последний пост не ищется (ошибка на единицу в
/// границе цикла), машина встаёт в очередь, хотя второй пост свободен.
static void TestSabotageOverflowIdlePost(void) {
    for (int sab = 0; sab <= 1; ++sab) {
        Config *cfg = LoadLineCfg(NULL);
        cfg->sabotage = sab ? SAB_OVERFLOW : SAB_NONE;
        Line *l = OpenLine();
        Car *a = NewCar(l, P_EXPRESS), *b = NewCar(l, P_EXPRESS);
        CHECK(Enter(l, S_TUNNEL, a) == ADMIT_SEAT);
        CHECK(Enter(l, S_TUNNEL, b) == (sab ? ADMIT_QUEUE : ADMIT_SEAT));
        CHECK(Queued(l, S_TUNNEL) == sab);
        int post = -1;
        CHECK(QueueTurn(l, S_TUNNEL, b, &post) == 0);  // при sab второй пост так и не ищется
        LineFree(l);
    }
}

/// Запросы момента покоя на dead-stage: машина в очереди wax, у которой нет постов.
static void TestStallQueries(void) {
    LoadLineCfg(DEAD_CFG);
    Line *l = OpenLine();
    Car *stuck = NewCar(l, P_PREMIUM);
    CHECK(Enter(l, S_WAX, stuck) == ADMIT_QUEUE);
    CHECK(LineProgressPossible(l) == 0);
    CHECK(LineStallCause(l) == S_WAX);
    CHECK(LineEntranceMayAccept(l) == 1);
    Car *busy = NewCar(l, P_EXPRESS);
    CHECK(Enter(l, S_TUNNEL, busy) == ADMIT_SEAT);
    CHECK(LineProgressPossible(l) == 1);  // моется
    MutexLock(&l->stage[S_TUNNEL].m);
    l->stage[S_TUNNEL].post[busy->post].seat[busy->seat].left = 0;  // помыта, ей нужна сушка
    MutexUnlock(&l->stage[S_TUNNEL].m);
    CHECK(LineProgressPossible(l) == 0);
    SetRepair(l, S_DRY, 1, 3);
    CHECK(LineProgressPossible(l) == 1);  // чинят пост, который ей нужен
    SetRepair(l, S_PREWASH, 0, 3);
    SetRepair(l, S_DRY, 1, 0);
    CHECK(LineProgressPossible(l) == 0);  // ремонт пустого поста, которого никто не ждёт
    LineFree(l);

    LoadLineCfg(ZERO_CFG);  // въезды обеих первых стадий заняты
    l = OpenLine();
    CHECK(Enter(l, S_PREWASH, NewCar(l, P_STANDARD)) == ADMIT_SEAT);
    CHECK(Enter(l, S_TUNNEL, NewCar(l, P_EXPRESS)) == ADMIT_SEAT);
    CHECK(LineEntranceMayAccept(l) == 1);
    CHECK(Enter(l, S_TUNNEL, NewCar(l, P_EXPRESS)) == ADMIT_SEAT);
    CHECK(LineEntranceMayAccept(l) == 0);
    CHECK(LineStallCause(l) == -1);
    LineFree(l);
}

// ---- Монитор стадии: машины - настоящие потоки, часы идут ----

/// Что видел крючок: число моментов покоя, минута последнего и состояние потока watch в нём.
typedef struct {
    int calls, lastNow;
    const SimThread *watch;
    SimState watchState;
    long watchParks;
    const Cond *watchCond;
} StageHookLog;

static void WatchThread(const SimThread *t, void *ctx) {
    StageHookLog *h = ctx;
    if (t == h->watch) {
        h->watchState = t->state;
        h->watchParks = t->parks;
        h->watchCond = t->waitCond;
    }
}

/// Крючок: запоминает покой; спящих нет - конец модели (ждущие выходят с SIM_STOP).
static int StageHook(int now, int nextWake, void *ctx) {
    StageHookLog *h = ctx;
    ++h->calls;
    h->lastNow = now;
    SimForEach(WatchThread, h);
    return nextWake != -1;
}

typedef enum { ACT_SEAT, ACT_RESERVE, ACT_DRIVER } ActKind;

/// Поток теста. ACT_SEAT - машина очереди (StageAwaitSeat), ACT_RESERVE - машина с поста стадии
/// from ждёт места (StageReserve), ACT_DRIVER - в минуту 1 делает act.
typedef struct {
    ActKind kind;
    Car *car;
    int stage, from;
    void (*act)(Line *l);
    SimThread th;  ///< только у ACT_DRIVER
    SimStatus got;
    int doneAt;  ///< минута, когда машина дождалась (-1 - не дождалась)
    pthread_t tid;
} Actor;

static SimThread *ActorThread(Actor *a) {
    return a->kind == ACT_DRIVER ? &a->th : &a->car->th;
}

static void *ActorMain(void *arg) {
    Actor *a = arg;
    SimThread *self = ActorThread(a);
    if (SimBegin(self) == SIM_OK) {
        if (a->kind == ACT_DRIVER) {
            a->got = SimSleep(self, 1);
            if (a->got == SIM_OK) {
                a->act(&line);
            }
        } else {
            Stage *st = &line.stage[a->stage];
            MutexLock(&st->m);
            a->got = a->kind == ACT_SEAT ? StageAwaitSeat(&line, st, a->car)
                                         : StageReserve(&line, st, a->car, a->from, 0);
            if (a->got == SIM_OK) {
                a->doneAt = SimNow();
            }
            MutexUnlock(&st->m);
        }
    }
    SimExit(self);
    return NULL;
}

/// Порождает потоки в порядке массива (в -D в нём же они и стартуют), затем часы; ждёт конца.
static void RunActors(Actor *a, int n) {
    for (int i = 0; i < n; ++i) {
        a[i].got = SIM_STOP;
        a[i].doneAt = -1;
        if (a[i].kind == ACT_DRIVER) {
            CHECK(SimThreadInit(&a[i].th, TH_POST, 100 + i, "driver", 0) == 0);
        }
        SimSpawned(ActorThread(&a[i]));
        CHECK(ThreadCreate(&a[i].tid, ActorMain, &a[i], 0) == 0);
    }
    pthread_t clock;
    CHECK(ThreadCreate(&clock, SimClockMain, NULL, 0) == 0);
    pthread_join(clock, NULL);
    for (int i = 0; i < n; ++i) {
        pthread_join(a[i].tid, NULL);
        if (a[i].kind == ACT_DRIVER) {
            SimThreadDestroy(&a[i].th);
        }
    }
}

static Car *actCars[4];  // машины для act водителя; пишет тест до запуска потоков

/// Водитель: машина actCars[0] на посту тоннеля доделала операцию и уезжает.
static void VacateFirst(Line *l) {
    Vacate(l, S_TUNNEL, actCars[0]);
}

/// пост тоннеля занят Express, в очереди Express (раньше) и Premium, rule=program.
/// Место от StageVacate получает Premium, кто бы ни проснулся первым.
static void TestQueueTurnByPriority(void) {
    int ok = 1;
    for (int run = 0; run < 35; ++run) {
        int det = run >= 30;  // 30 повторов в обычном режиме и 5 в -D
        Config *cfg = LoadLineCfg(PRIORITY_CFG);
        cfg->rule = PRIO_PROGRAM;
        StageHookLog h = {0};
        SimOptions o = {.deterministic = det, .hook = StageHook, .ctx = &h};
        Line *l = OpenLineWith(&o);
        Car *busy = NewCar(l, P_EXPRESS), *express = NewCar(l, P_EXPRESS);
        Car *premium = NewCar(l, P_PREMIUM);
        CHECK(Enter(l, S_TUNNEL, busy) == ADMIT_SEAT);
        CHECK(Enter(l, S_TUNNEL, express) == ADMIT_QUEUE);
        CHECK(Enter(l, S_TUNNEL, premium) == ADMIT_QUEUE);
        actCars[0] = busy;
        Actor a[3] = {{.kind = ACT_SEAT, .car = express, .stage = S_TUNNEL},
                      {.kind = ACT_SEAT, .car = premium, .stage = S_TUNNEL},
                      {.kind = ACT_DRIVER, .act = VacateFirst}};
        RunActors(a, 3);
        ok = ok && a[1].got == SIM_OK && a[1].doneAt == 1 && premium->place == CAR_POST;
        ok = ok && a[0].got == SIM_STOP && express->place == CAR_QUEUE;
        ok = ok && Queued(l, S_TUNNEL) == 1 && h.lastNow == 1;
        LineFree(l);
    }
    CHECK(ok);
}

/// Водитель: в списке ожидающих actCars[2] стоит раньше actCars[3], но номер у неё теперь
/// больше; затем в одну минуту освобождаются оба поста (actCars[0], actCars[1]).
static void FreeTwoPosts(Line *l) {
    Stage *st = &l->stage[S_TUNNEL];
    MutexLock(&st->m);
    long t = actCars[2]->ticket;
    actCars[2]->ticket = actCars[3]->ticket;
    actCars[3]->ticket = t;
    MutexUnlock(&st->m);
    Vacate(l, S_TUNNEL, actCars[0]);
    Vacate(l, S_TUNNEL, actCars[1]);
}

/// две машины ждут room тоннеля zero-buffer (два занятых поста, очереди нет);
/// оба поста освобождаются в одну минуту - обе брони получены в эту же минуту. В -D первой
/// просыпается не первая по номеру: она снова засыпает, и разбудить её должна получившая бронь.
static void TestRoomTwoFreed(void) {
    int ok = 1;
    for (int run = 0; run < 15; ++run) {
        int det = run >= 10;
        LoadLineCfg(ZERO_CFG);
        StageHookLog h = {0};
        SimOptions o = {.deterministic = det, .hook = StageHook, .ctx = &h};
        Line *l = OpenLineWith(&o);
        Car *t1 = NewCar(l, P_EXPRESS), *t2 = NewCar(l, P_EXPRESS);
        Car *w1 = NewCar(l, P_STANDARD), *w2 = NewCar(l, P_STANDARD);
        CHECK(Enter(l, S_TUNNEL, t1) == ADMIT_SEAT && Enter(l, S_TUNNEL, t2) == ADMIT_SEAT);
        actCars[0] = t1;
        actCars[1] = t2;
        actCars[2] = w1;
        actCars[3] = w2;
        Actor a[3] = {{.kind = ACT_RESERVE, .car = w1, .stage = S_TUNNEL, .from = S_PREWASH},
                      {.kind = ACT_RESERVE, .car = w2, .stage = S_TUNNEL, .from = S_PREWASH},
                      {.kind = ACT_DRIVER, .act = FreeTwoPosts}};
        RunActors(a, 3);
        ok = ok && a[0].got == SIM_OK && a[1].got == SIM_OK;
        ok = ok && a[0].doneAt == 1 && a[1].doneAt == 1 && h.lastNow == 1;
        ok = ok && w1->res == RES_SEAT && w2->res == RES_SEAT && w1->resPost != w2->resPost;
        ok = ok && w1->ticket == -1 && w2->ticket == -1 && Waiters(l, S_TUNNEL) == 0;
        int b1 = FindEv(EV_BLOCKED, w1->id), b2 = FindEv(EV_BLOCKED, w2->id);
        ok = ok && b1 >= 0 && evLog[b1].stage == S_PREWASH && evLog[b1].arg == S_TUNNEL;
        ok = ok && b2 >= 0 && evLog[b2].post == 0 && evLog[b2].time == 0;
        LineFree(l);
    }
    CHECK(ok);
}

/// Стадия b: один пост, очередь 2; rule=program.
static const char *WAKE_CFG = "stage = a 1 3 5 2\nstage = b 1 3 5 2\n"
                              "program = Express 50 1 a b\nprogram = Premium 50 3 a b\n"
                              "priority = program\n";

/// Водитель: машина actCars[0] уезжает с поста стадии b.
static void VacateOnB(Line *l) {
    Vacate(l, 1, actCars[0]);
}

/// WakeRoom не трогает машину, ждущую seatFree той же стадии. -D: пост b освобождается, обе
/// машины очереди просыпаются; Express (раньше в очереди) видит, что не её очередь, и снова
/// засыпает; Premium садится, место в очереди освобождается - WakeRoom будит ожидающего с a.
/// Express при этом остаётся ST_PARKED на seatFree: засыпала ровно два раза.
static void TestWakeOnlyListed(void) {
    for (int det = 1; det >= 0; --det) {
        LoadLineCfg(WAKE_CFG);
        StageHookLog h = {0};
        SimOptions o = {.deterministic = det, .hook = StageHook, .ctx = &h};
        Line *l = OpenLineWith(&o);
        Car *busy = NewCar(l, 0), *express = NewCar(l, 0), *premium = NewCar(l, 1);
        Car *w = NewCar(l, 0);
        CHECK(Enter(l, 1, busy) == ADMIT_SEAT);
        CHECK(Enter(l, 1, express) == ADMIT_QUEUE && Enter(l, 1, premium) == ADMIT_QUEUE);
        actCars[0] = busy;
        h.watch = &express->th;
        Actor a[4] = {{.kind = ACT_SEAT, .car = express, .stage = 1},
                      {.kind = ACT_SEAT, .car = premium, .stage = 1},
                      {.kind = ACT_RESERVE, .car = w, .stage = 1, .from = 0},
                      {.kind = ACT_DRIVER, .act = VacateOnB}};
        RunActors(a, 4);
        CHECK(a[1].doneAt == 1 && premium->place == CAR_POST);
        CHECK(a[2].doneAt == 1 && w->res == RES_QUEUE);
        CHECK(a[0].got == SIM_STOP && express->place == CAR_QUEUE);
        CHECK(h.lastNow == 1 && h.watchState == ST_PARKED);
        CHECK(h.watchCond == &l->stage[1].seatFree);
        if (det) {
            CHECK(h.watchParks == 2);  // в минуту 0 и после пробуждения seatFree в минуту 1
        }
        LineFree(l);
    }
}

// ---- Граф ожидания, таблица потоков, снимок ----

/// Что функция вывода пишет в дескриптор: через pipe (вывод маленький, буфера хватает).
static char graphOut[8192];

static const char *Capture(void (*print)(int fd, Line *l, int color), Line *l) {
    int fds[2];
    CHECK(pipe(fds) == 0);
    print(fds[1], l, 0);
    close(fds[1]);
    ssize_t n = read(fds[0], graphOut, sizeof graphOut - 1);
    graphOut[n > 0 ? n : 0] = '\0';
    close(fds[0]);
    return graphOut;
}

static void PrintThreads(int fd, Line *l, int color) {
    (void)color;
    WaitGraphThreads(fd, l->cfg);
}

/// Помытая машина на посту стадии s просит место на стадии next: места нет, и она встаёт в
/// список ожидающих (как StageReserve, но без ожидания - потока у машины нет).
static void Block(Line *l, int s, Car *car, int next) {
    MutexLock(&l->stage[s].m);
    l->stage[s].post[car->post].seat[car->seat].left = 0;
    MutexUnlock(&l->stage[s].m);
    CHECK(RoomTurn(l, next, car) == RES_NONE);
    ListWaiter(l, next, car, atomic_fetch_add(&l->nextTicket, 1) + 1);
}

/// Пример: wax без постов, перед ним всё забито.
static Line *StuckLine(void) {
    LoadLineCfg(DEAD_CFG);
    Line *l = OpenLine();
    CHECK(Enter(l, S_WAX, NewCar(l, P_PREMIUM)) == ADMIT_QUEUE);  // #1
    CHECK(Enter(l, S_WAX, NewCar(l, P_PREMIUM)) == ADMIT_QUEUE);  // #2
    Car *c3 = NewCar(l, P_PREMIUM), *c4 = NewCar(l, P_PREMIUM);
    CHECK(Enter(l, S_TUNNEL, c3) == ADMIT_SEAT);
    CHECK(Enter(l, S_TUNNEL, c4) == ADMIT_SEAT);
    Block(l, S_TUNNEL, c3, S_WAX);
    Block(l, S_TUNNEL, c4, S_WAX);
    Car *c5 = NewCar(l, P_STANDARD);
    CHECK(Enter(l, S_PREWASH, c5) == ADMIT_SEAT);
    CHECK(Enter(l, S_TUNNEL, NewCar(l, P_EXPRESS)) == ADMIT_QUEUE);  // #6
    CHECK(Enter(l, S_TUNNEL, NewCar(l, P_EXPRESS)) == ADMIT_QUEUE);  // #7
    Block(l, S_PREWASH, c5, S_TUNNEL);
    return l;
}

static void TestWaitGraphText(void) {
    Line *l = StuckLine();
    const char *g = Capture(WaitGraphPrint, l);
    CHECK(strstr(g, "WAITS-FOR GRAPH at 08:00 (7 car(s) stuck)\n") == g);
    CHECK(strstr(g, "  car #1   in wax queue       -> waits for a post in wax\n"));
    CHECK(strstr(g, "  car #3   on tunnel post 1   -> waits for room in wax\n"));
    CHECK(strstr(g, "  car #4   on tunnel post 2   -> waits for room in wax\n"));
    CHECK(strstr(g, "    wax: 0 posts                <- ROOT CAUSE: stage wax has no posts\n"));
    CHECK(strstr(g, "  car #5   on prewash post 1  -> waits for room in tunnel\n"));
    CHECK(strstr(g, "  car #6   in tunnel queue    -> waits for a post in tunnel\n"));
    CHECK(strstr(g, "    tunnel: 2/2 posts held by car #3, car #4 (both blocked), queue 2/2\n"));
    const char *wax = strstr(g, "wax: 0 posts"), *c5 = strstr(g, "car #5 ");
    CHECK(wax && c5 && wax < c5);  // сначала то, что ближе к корню (дальше по линии)
    int roots = 0;
    for (const char *p = strstr(g, "ROOT CAUSE"); p; p = strstr(p + 1, "ROOT CAUSE")) {
        ++roots;
    }
    CHECK(roots == 1);  // корень назван один раз
    LineFree(l);
}

static void TestWaitGraphBroken(void) {
    LoadLineCfg(DEAD_CFG);
    Line *l = OpenLine();
    SetRepair(l, S_DRY, 0, 6);
    SetRepair(l, S_DRY, 1, 4);
    CHECK(Enter(l, S_DRY, NewCar(l, P_EXPRESS)) == ADMIT_QUEUE);
    const char *g = Capture(WaitGraphPrint, l);
    CHECK(strstr(g, "(1 car(s) stuck)"));
    CHECK(strstr(g, "  car #1   in dry queue       -> waits for a post in dry\n"));
    CHECK(strstr(g, "    dry: 2/2 posts broken       <- ROOT CAUSE: all dry posts are broken "
                    "(repair 4 min)\n"));
    LineFree(l);
}

static void TestWaitGraphDot(void) {
    Line *l = StuckLine();
    char path[] = "/tmp/cwmon-dot-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);
    CHECK(WaitGraphDot(l, path) == 0);
    int in = open(path, O_RDONLY);
    char buf[4096] = "";
    ssize_t n = read(in, buf, sizeof buf - 1);
    buf[n > 0 ? n : 0] = '\0';
    close(in);
    unlink(path);
    CHECK(strncmp(buf, "digraph", 7) == 0);
    CHECK(strstr(buf, "\"car #3\" -> \"stage wax\""));
    CHECK(strstr(buf, "\"stage tunnel\" -> \"car #3\""));
    CHECK(strstr(buf, "ROOT CAUSE"));
    CHECK(WaitGraphDot(l, "/nonexistent/dir/g.dot") == -1);
    LineFree(l);
}

static void TestThreadTable(void) {
    Line *l = StuckLine();
    const char *t = Capture(PrintThreads, l);
    CHECK(strncmp(t, "THREADS", 7) == 0);
    CHECK(strstr(t, "coordinator"));
    CHECK(strstr(t, "post tunnel.1"));
    CHECK(strstr(t, "car #7"));
    LineFree(l);
}

/// Состояние потока машины, как будто она ждёт в SimWaitOn с такой причиной.
static void ParkCar(Car *car, WaitKind kind, int stage) {
    car->th.state = ST_PARKED;
    car->th.why = (WaitReason){.kind = kind, .stage = stage, .post = 0};
}

/// Все машины снова «завершены»: иначе LineFree не даст уничтожить их потоки.
static void UnparkCars(Line *l) {
    int next = 0;
    for (Car *c = LineCar(l, next); c != NULL; c = LineCar(l, next)) {
        c->th.state = ST_DONE;
        next = c->id + 1;
    }
}

static void TestSnapshotPanel(void) {
    Line *l = StuckLine();  // #1, #2 в очереди wax; #3, #4 заблокированы на tunnel; #5 на prewash
    ParkCar(LineCar(l, 0), WAIT_QUEUE, S_WAX);
    ParkCar(LineCar(l, 2), WAIT_ROOM, S_WAX);
    ParkCar(LineCar(l, 3), WAIT_ROOM, S_WAX);
    ParkCar(LineCar(l, 4), WAIT_ROOM, S_TUNNEL);
    LineCar(l, 1)->th.state = ST_SLEEPING;
    Snapshot s;
    SnapshotTake(l, &s);
    CHECK(s.threads[TH_CAR] == 5);
    CHECK(s.phase[TH_CAR][PH_ROOM] == 3);
    CHECK(s.phase[TH_CAR][PH_QUEUE] == 1);
    CHECK(s.phase[TH_CAR][PH_SLEEP] == 1);
    CHECK(s.panelCars == 4);
    CHECK(s.panel[0].car == 0 && s.panel[0].why == WAIT_QUEUE);  // по возрастанию номера
    CHECK(s.panel[1].car == 2 && s.panel[1].why == WAIT_ROOM && s.panel[1].stage == S_WAX);
    CHECK(s.queueProg[S_WAX][0] == P_PREMIUM);
    CHECK(s.arrived == 7 && s.queued[S_TUNNEL] == 2);

    // тесно: панель на PANEL_CARS мест, машины с WAIT_ROOM вытесняют ждущих в очереди
    for (int i = 0; i < 10; ++i) {
        ParkCar(NewCar(l, P_EXPRESS), WAIT_QUEUE, S_TUNNEL);
    }
    ParkCar(NewCar(l, P_EXPRESS), WAIT_ROOM, S_DRY);
    SnapshotTake(l, &s);
    int rooms = 0;
    for (int i = 0; i < s.panelCars; ++i) {
        rooms += s.panel[i].why == WAIT_ROOM;
    }
    CHECK(s.panelCars == PANEL_CARS);
    CHECK(rooms == 4);  // все четыре заблокированные видны
    UnparkCars(l);
    LineFree(l);
}

/// Что ViewLiveDraw нарисовал за один кадр: через pipe (кадр меньше его буфера).
static char frameOut[16384];

static void TestLiveDrawText(void) {
    Line *l = StuckLine();
    ParkCar(LineCar(l, 2), WAIT_ROOM, S_WAX);
    int fds[2];
    CHECK(pipe(fds) == 0);
    ViewLiveInit(fds[1], l->cfg);
    ViewLiveEnd();
    ssize_t n = read(fds[0], frameOut, sizeof frameOut - 1);  // очистка экрана и курсор
    CHECK(n > 0);
    Snapshot s;
    SnapshotTake(l, &s);
    ViewLiveDraw(&s);
    n = read(fds[0], frameOut, sizeof frameOut - 1);
    frameOut[n > 0 ? n : 0] = '\0';
    CHECK(strstr(frameOut, "THREADS"));
    CHECK(strstr(frameOut, "car #3 "));
    CHECK(strstr(frameOut, "blocked") && strstr(frameOut, "-> room in wax"));
    CHECK(strstr(frameOut, "no posts"));  // у wax нет постов
    CHECK(strstr(frameOut, "entrance"));
    close(fds[0]);
    close(fds[1]);
    UnparkCars(l);
    LineFree(l);
}

static void TestRenderFrames(void) {
    Config c;
    LoadNormal(&c);
    int fds[2];
    CHECK(pipe(fds) == 0);
    ViewLiveInit(fds[1], &c);
    CHECK(RenderStart() == 0);
    CHECK(RenderActive() == 1);
    CHECK(RenderStart() == 0);  // повторный вызов ничего не делает
    Snapshot s;
    memset(&s, 0, sizeof s);
    for (int minute = 0; minute < 3; ++minute) {
        s.now = minute;
        RenderSubmit(&s);
    }
    RenderFlush();
    RenderStop();
    CHECK(RenderActive() == 0);
    ssize_t n = read(fds[0], frameOut, sizeof frameOut - 1);
    frameOut[n > 0 ? n : 0] = '\0';
    int frames = 0;
    for (const char *p = frameOut; (p = strstr(p, "\033[H")) != NULL; p += 3) {
        ++frames;
    }
    CHECK(frames == 3);
    close(fds[0]);
    close(fds[1]);
}

/// Остановка без единого кадра: поток ждёт кадр на notEmpty, RenderStop его будит.
static void TestRenderStopWhileWaiting(void) {
    Config c;
    LoadNormal(&c);
    ViewLiveInit(open("/dev/null", O_WRONLY), &c);
    struct timespec t0, t1;
    CHECK(RenderStart() == 0);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    RenderStop();
    clock_gettime(CLOCK_MONOTONIC, &t1);
    CHECK(t1.tv_sec - t0.tv_sec < 1);
    RenderStop();  // повторно безопасно
    CHECK(RenderActive() == 0);
}

// ---- Аудитор: события подаются прямо в AuditorListener, без шины ----

static Config auditCfg;
static int auditNull = -1;  ///< /dev/null: сообщения о нарушениях в тестах не нужны

/// Свежий аудитор на линии «a (1 пост, очередь 1) -> b (1 пост ёмкостью 2, очередь 1)».
static void AuditStart(void) {
    char err[256];
    ConfigDefaults(&auditCfg);
    CHECK(ConfigParseText(&auditCfg,
                          "stage = a 1 1 2 1\nstage = b 1 1 2 1 2\nprogram = P 1 1 a b\n", err,
                          sizeof err) == 0);
    if (auditNull < 0) {
        auditNull = open("/dev/null", O_WRONLY);
    }
    AuditorInit(auditNull, &auditCfg);
}

static void Feed(EventType type, int car, int stage, int post, int arg) {
    Event e = {.type = type, .car = car, .stage = stage, .post = post, .program = 0, .arg = arg};
    AuditorListener(&e);
}

/// Машина приезжает и садится на пост a.
static void FeedArriveOnA(int car) {
    Feed(EV_ARRIVE, car, -1, -1, 0);
    Feed(EV_PROGRAM, car, -1, -1, 0);
    Feed(EV_ENTER, car, 0, 0, 0);
    Feed(EV_OP_START, car, 0, 0, 1);
}

static void TestAuditorCleanRun(void) {
    AuditStart();
    FeedArriveOnA(0);
    Feed(EV_OP_END, 0, 0, 0, 0);
    Feed(EV_LEAVE, 0, 0, 0, 0);
    // между LEAVE и MOVE машины 0 на освободившийся пост садится машина 1 из очереди
    Feed(EV_ARRIVE, 1, -1, -1, 0);
    Feed(EV_PROGRAM, 1, -1, -1, 0);
    Feed(EV_QUEUE, 1, 0, -1, 1);
    Feed(EV_ENTER, 1, 0, 0, 0);
    Feed(EV_OP_START, 1, 0, 0, 1);
    Feed(EV_MOVE, 0, 1, -1, 0);
    Feed(EV_ENTER, 0, 1, 0, 0);
    Feed(EV_OP_START, 0, 1, 0, 1);
    Feed(EV_LEAVE, 1, 0, 0, 0);
    Feed(EV_MOVE, 1, 1, -1, 0);
    Feed(EV_ENTER, 1, 1, 0, 0);  // ёмкость b - 2: вторая машина на том же посту законна
    Feed(EV_OP_START, 1, 1, 0, 1);
    Feed(EV_LEAVE, 0, 1, 0, 0);
    Feed(EV_DONE, 0, 1, -1, 5);
    Feed(EV_ARRIVE, 2, -1, -1, 0);
    Feed(EV_REJECT, 2, 0, -1, 1);
    CHECK(AuditorViolations() == 0);
    CHECK(AuditorChecks() == 22);
}

static void TestAuditorCapacity(void) {
    AuditStart();
    FeedArriveOnA(0);
    FeedArriveOnA(1);  // у поста a ёмкость 1
    CHECK(AuditorViolations() == 1);
}

static void TestAuditorTwoPlaces(void) {
    AuditStart();
    FeedArriveOnA(0);
    Feed(EV_MOVE, 0, 1, -1, 0);  // переезд без EV_LEAVE: машина всё ещё на посту a
    CHECK(AuditorViolations() == 1);

    AuditStart();
    FeedArriveOnA(0);
    Feed(EV_LEAVE, 0, 0, 0, 0);
    Feed(EV_QUEUE, 0, 1, -1, 1);  // в очередь b без EV_MOVE
    CHECK(AuditorViolations() == 1);

    AuditStart();
    FeedArriveOnA(0);
    Feed(EV_LEAVE, 0, 0, 0, 0);
    Feed(EV_LEAVE, 0, 0, 0, 0);  // второй раз с того же поста
    CHECK(AuditorViolations() == 1);
}

static void TestAuditorRoute(void) {
    AuditStart();
    Feed(EV_ARRIVE, 0, -1, -1, 0);
    Feed(EV_PROGRAM, 0, -1, -1, 0);
    Feed(EV_ENTER, 0, 1, 0, 0);  // маршрут начинается с a
    CHECK(AuditorViolations() == 1);

    AuditStart();
    FeedArriveOnA(0);
    Feed(EV_LEAVE, 0, 0, 0, 0);
    Feed(EV_DONE, 0, 0, -1, 3);  // уехала, пройдя 1 стадию из 2
    CHECK(AuditorViolations() == 1);
}

static void TestAuditorBrokenPost(void) {
    AuditStart();
    Feed(EV_FAIL, -1, 0, 0, 5);
    FeedArriveOnA(0);  // операция на сломанном посту
    CHECK(AuditorViolations() == 1);

    AuditStart();
    Feed(EV_FAIL, -1, 0, 0, 5);
    Feed(EV_REPAIR, -1, 0, 0, 0);
    FeedArriveOnA(0);
    CHECK(AuditorViolations() == 0);
}

static void TestAuditorQueue(void) {
    AuditStart();
    for (int car = 0; car < 2; ++car) {
        Feed(EV_ARRIVE, car, -1, -1, 0);
        Feed(EV_PROGRAM, car, -1, -1, 0);
        Feed(EV_QUEUE, car, 0, -1, car + 1);  // очередь a вмещает одну машину
    }
    CHECK(AuditorViolations() == 1);
    AuditorFree();
}

/// Код выхода: нарушение (аудитор, LOCKDEP или SOFT_ASSERT) важнее исхода модели.
static void TestExitCode(void) {
    CHECK(LineExitCode(LINE_FINISHED, 0, 0, 0) == 0);
    CHECK(LineExitCode(LINE_STALLED, 0, 0, 0) == 2);
    CHECK(LineExitCode(LINE_INTERRUPTED, 0, 0, 0) == 3);
    CHECK(LineExitCode(LINE_NO_RESOURCES, 0, 0, 0) == 5);
    CHECK(LineExitCode(LINE_FINISHED, 1, 0, 0) == 4);
    CHECK(LineExitCode(LINE_FINISHED, 0, 1, 0) == 4);
    CHECK(LineExitCode(LINE_FINISHED, 0, 0, 1) == 4);  // одного SOFT_ASSERT достаточно
    CHECK(LineExitCode(LINE_STALLED, 0, 0, 2) == 4);
}

/// Крючок выхода для TestFatalExit: метка в stdout (дочернего процесса).
static void MarkHook(void) {
    if (write(1, "hook\n", 5) < 0) {
        _exit(6);  // метку не записать - тест увидит другой код выхода
    }
}

/// FatalExit: сначала крючок выхода (вернуть терминал), затем _exit с кодом.
static void TestFatalExit(void) {
    int fds[2];
    CHECK(pipe(fds) == 0);
    fflush(NULL);  // буфер stdout родителя не должен попасть в pipe из дочернего процесса
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1);
        LockdepSetExitHook(MarkHook);
        FatalExit(5);
    }
    close(fds[1]);
    int status = 0;
    waitpid(pid, &status, 0);
    char buf[16] = "";
    ssize_t n = read(fds[0], buf, sizeof buf - 1);
    close(fds[0]);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 5);
    CHECK(n >= 5 && strncmp(buf, "hook\n", 5) == 0);  // крючок отработал до выхода
}

/// Покой без спящих, а крючок сказал бы «продолжать»: разбудить некому - это тупик, а не вечное
/// ожидание. События крючка подписаны потоком часов ("clock").
static void TestQuiesceNobodyToWake(void) {
    LoadLineCfg(NULL);
    Line *l = OpenLine();
    EventsSubscribe(SourceListener);
    Car *a = NewCar(l, P_EXPRESS);
    CHECK(Enter(l, S_TUNNEL, a) == ADMIT_SEAT);  // операция идёт: движение возможно
    atomic_store(&l->open, 0);
    atomic_store(&l->inside, 1);
    CHECK(QuiesceHook(5, 6, l) == 1);  // есть кому проснуться - продолжаем
    long before = SoftAssertFailures();
    srcSeen[0] = '\0';
    CHECK(QuiesceHook(5, -1, l) == 0);
    CHECK(l->status == LINE_STALLED);
    CHECK(SoftAssertFailures() == before + 1);
    CHECK(FindEv(EV_STALL, -1) >= 0);
    CHECK(strcmp(srcSeen, "clock") == 0);
    EventsSetSource(NULL);
    GanttFree();
    LineFree(l);
}

static atomic_int twoSubmitted = 0;

static void *SubmitTwo(void *unused) {
    (void)unused;
    Snapshot s;
    memset(&s, 0, sizeof s);
    RenderSubmit(&s);
    RenderSubmit(&s);
    atomic_store(&twoSubmitted, 1);
    return NULL;
}

static void *DrainPipe(void *arg) {
    int fd = *(int *)arg;
    char buf[4096];
    while (read(fd, buf, sizeof buf) > 0) {
    }
    return NULL;
}

/// Два буфера по-настоящему: пока первый кадр рисуется (вывод застрял), второй уже кладётся.
static void TestRenderDoubleBuffer(void) {
    Config c;
    LoadNormal(&c);
    int fds[2];
    CHECK(pipe(fds) == 0);
    ViewLiveInit(fds[1], &c);  // пишет очистку экрана - до того, как pipe забит
    // забиваем pipe, чтобы отрисовка первого кадра встала на write
    int flags = fcntl(fds[1], F_GETFL);
    fcntl(fds[1], F_SETFL, flags | O_NONBLOCK);  // NOLINT(bugprone-signed-bitwise)
    char junk[4096];
    memset(junk, 'x', sizeof junk);
    while (write(fds[1], junk, sizeof junk) > 0) {
    }
    fcntl(fds[1], F_SETFL, flags);
    CHECK(RenderStart() == 0);
    atomic_store(&twoSubmitted, 0);
    pthread_t sub, drain;
    CHECK(pthread_create(&sub, NULL, SubmitTwo, NULL) == 0);
    int ok = 0;
    for (int i = 0; i < 500 && !(ok = atomic_load(&twoSubmitted)); ++i) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L};
        nanosleep(&ts, NULL);
    }
    CHECK(ok);  // второй Submit не ждал, пока нарисуют первый
    CHECK(pthread_create(&drain, NULL, DrainPipe, &fds[0]) == 0);
    pthread_join(sub, NULL);
    RenderFlush();  // ждёт оба кадра
    RenderStop();
    close(fds[1]);
    pthread_join(drain, NULL);
    close(fds[0]);
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
    RUN(TestGrantFromHook);
    RUN(TestWaitOnWithExtraLock);
    RUN(TestDeterministicGrantOrder);
    RUN(TestStopWithTokenWaiters);
    RUN(TestClockWaitsStartBarrier);
    RUN(TestKickRerunsHook);
    RUN(TestKickCutsDelay);
    RUN(TestConcurrentEmitOrdered);
    RUN(TestDescribeTexts);
    RUN(TestSourceNames);
    RUN(TestViewLogFormat);
    RUN(TestAdmitDirectSeat);
    RUN(TestAdmitQueueThenFull);
    RUN(TestCapacityTwo);
    RUN(TestReservationCounts);
    RUN(TestPriorityPop);
    RUN(TestWaitersOrder);
    RUN(TestBrokenPostTakesNobody);
    RUN(TestZeroQueue);
    RUN(TestSabotageOverflow);
    RUN(TestSabotageDeadlock);
    RUN(TestSabotageOverflowIdlePost);
    RUN(TestStallQueries);
    RUN(TestQueueTurnByPriority);
    RUN(TestRoomTwoFreed);
    RUN(TestWakeOnlyListed);
    RUN(TestWaitGraphText);
    RUN(TestWaitGraphBroken);
    RUN(TestWaitGraphDot);
    RUN(TestThreadTable);
    RUN(TestSnapshotPanel);
    RUN(TestLiveDrawText);
    RUN(TestRenderFrames);
    RUN(TestRenderStopWhileWaiting);
    RUN(TestAuditorCleanRun);
    RUN(TestAuditorCapacity);
    RUN(TestAuditorTwoPlaces);
    RUN(TestAuditorRoute);
    RUN(TestAuditorBrokenPost);
    RUN(TestAuditorQueue);
    RUN(TestExitCode);
    RUN(TestFatalExit);
    RUN(TestQuiesceNobodyToWake);
    RUN(TestRenderDoubleBuffer);
    return UnitSummary();
}
