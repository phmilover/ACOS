#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "mutex.h"
#include "rng.h"
#include "sem.h"
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

typedef struct {
    Sem go, done;
    int flag;
} SemCtx;

static void *SemWorker(void *arg) {
    SemCtx *c = arg;
    SemWait(&c->go);
    c->flag = 1;
    SemPost(&c->done);
    return NULL;
}

static void TestSemAcrossThreads(void) {
    SemCtx c = {.flag = 0};
    CHECK(SemOpen(&c.go, 0) == 0);
    CHECK(SemOpen(&c.done, 0) == 0);
    pthread_t t;
    CHECK(pthread_create(&t, NULL, SemWorker, &c) == 0);
    SemPost(&c.go);
    SemWait(&c.done);
    CHECK(c.flag == 1);
    pthread_join(t, NULL);
    SemClose(&c.go);
    SemClose(&c.done);
}

static void TestSemTryWait(void) {
    Sem s;
    CHECK(SemOpen(&s, 1) == 0);
    CHECK(SemTryWait(&s) == 0);
    CHECK(SemTryWait(&s) == -1);
    SemPost(&s);
    CHECK(SemTryWait(&s) == 0);
    SemClose(&s);
}

static void TestSemNoLeak(void) {
    long before = SemOpenCount();
    Sem s;
    CHECK(SemOpen(&s, 0) == 0);
    CHECK(SemOpenCount() == before + 1);
    SemClose(&s);
    CHECK(SemOpenCount() == before);
}

static void TestSemFailHook(void) {
    Sem s;
    long before = SemOpenCount();
    setenv("CARWASH_FAIL_SEM_AFTER", "0", 1);  // NOLINT(concurrency-mt-unsafe)
    CHECK(SemOpen(&s, 0) == -1);
    CHECK(errno == ENOSPC);
    unsetenv("CARWASH_FAIL_SEM_AFTER");  // NOLINT(concurrency-mt-unsafe)
    // крючок перечитывается при каждом вызове
    CHECK(SemOpen(&s, 0) == 0);
    SemClose(&s);
    CHECK(SemOpenCount() == before);
}

int main(void) {
    RUN(TestStageCapacity);
    RUN(TestBadCapacity);
    RUN(TestRngStreams);
    RUN(TestSabotageNames);
    RUN(TestLockdepCatchesNesting);
    RUN(TestSemAcrossThreads);
    RUN(TestSemTryWait);
    RUN(TestSemNoLeak);
    RUN(TestSemFailHook);
    return UnitSummary();
}
