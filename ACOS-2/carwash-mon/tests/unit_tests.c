#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "barrier.h"
#include "config.h"
#include "rng.h"
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
    return UnitSummary();
}
