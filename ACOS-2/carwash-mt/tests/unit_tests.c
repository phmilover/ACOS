#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "mutex.h"
#include "rng.h"
#include "sem.h"
#include "sim.h"
#include "soft_assert.h"
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

// ---- виртуальные часы ----

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
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L};  // окно, чтобы поймать
            nanosleep(&ts, NULL);                                     // второго работающего
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
    int notQuiet;    // потоки в RUNNING/READY, замеченные через SimForEach
    int seen;        // сколько потоков обошёл SimForEach за последний вызов
    int idleRounds;  // сколько раз при nextWake == -1 вернуть 1 (и дать знак через idle)
    int handshake;   // 1: в первый такой раз ждать в крючке resume (будят во время крючка)
    Sem idle, resume;
} HookLog;

static void CountQuiet(const SimThread *t, void *ctx) {
    HookLog *h = ctx;
    ++h->seen;
    if (t->state == ST_RUNNING || t->state == ST_READY) {
        ++h->notQuiet;
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
    if (nextWake == -1 && h->idleRounds > 0) {
        --h->idleRounds;
        SemPost(&h->idle);
        if (h->handshake) {
            h->handshake = 0;
            SemWait(&h->resume);
        }
        return 1;
    }
    return nextWake != -1;
}

/// SimInit для очередного теста: семафор часов прошлой модели закрыт, открыт ровно один новый.
/// @param extra сколько своих семафоров тест держит открытыми сверх обычного
static long FreshSim(const SimOptions *o, long extra) {
    static long baseline = -1;  // открытых семафоров сразу после первого SimInit
    CHECK(SimInit(o) == 0);
    long open = SemOpenCount();
    if (baseline < 0) {
        baseline = open - extra;
    }
    CHECK(open - extra == baseline);
    return open;
}

/// Готовит модель, порождает потоки (часы последними), ждёт конца и всё убирает.
static void RunWorkers(Worker *w, int n, int deterministic, HookLog *h) {
    SimOptions o = {.deterministic = deterministic, .delayMs = 0, .hook = RecordHook, .ctx = h};
    long sems = FreshSim(&o, 0);
    pthread_t tid[MAX_WORKERS], clock;
    for (int i = 0; i < n; ++i) {
        CHECK(SimThreadInit(&w[i].th, TH_POST, w[i].th.id, "post", w[i].th.order) == 0);
        SimSpawned(&w[i].th);
        CHECK(pthread_create(&tid[i], NULL, WorkerMain, &w[i]) == 0);
    }
    CHECK(pthread_create(&clock, NULL, SimClockMain, NULL) == 0);
    pthread_join(clock, NULL);
    for (int i = 0; i < n; ++i) {
        pthread_join(tid[i], NULL);
        SimThreadDestroy(&w[i].th);
    }
    CHECK(SemOpenCount() == sems);
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

typedef struct {
    SimThread a, b;
    int aSaw;
    int extraWakes;  // сколько раз сняли вторую и дальнейшие парковки
    SimStatus aStatus, again, sleepAgain;
    Mutex *hold;  // если не NULL, A паркуется, держа этот мьютекс
} PairCtx;

static void *ParkerMain(void *arg) {
    PairCtx *c = arg;
    if (SimBegin(&c->a) == SIM_OK) {
        WaitReason op = {.kind = WAIT_OPERATION, .stage = 1, .post = 0};
        if (c->hold) {
            MutexLock(c->hold);
            c->aStatus = SimPark(&c->a, op);  // нарушение: парковка под мьютексом
            MutexUnlock(c->hold);
        } else {
            c->aStatus = SimPark(&c->a, op);
        }
        c->aSaw = SimNow();
        while ((c->again = SimPark(&c->a, (WaitReason){.kind = WAIT_QUEUE})) == SIM_OK) {
            ++c->extraWakes;
        }
        c->sleepAgain = SimSleep(&c->a, 1);
    }
    SimExit(&c->a);
    return NULL;
}

static void *WakerMain(void *arg) {
    PairCtx *c = arg;
    if (SimBegin(&c->b) == SIM_OK && SimSleep(&c->b, 2) == SIM_OK) {
        SimWake(&c->a);
    }
    SimExit(&c->b);
    return NULL;
}

/// A паркуется, пока его не остановят; B будит его через 2 минуты, крючок кончает модель,
/// когда спящих нет. Если @p withB = 0, A дважды будит главный поток: в первом покое - во
/// время крючка, во втором - когда часы уже ждут.
static void RunPair(PairCtx *c, HookLog *h, int withB) {
    SimOptions o = {.deterministic = 0, .delayMs = 0, .hook = RecordHook, .ctx = h};
    long sems = FreshSim(&o, withB ? 0 : 2);  // без B тест держит открытыми idle и resume
    pthread_t ta, tb, clock;
    CHECK(SimThreadInit(&c->a, TH_CAR, 1, "car A", 0) == 0);
    SimSpawned(&c->a);
    CHECK(pthread_create(&ta, NULL, ParkerMain, c) == 0);
    if (withB) {
        CHECK(SimThreadInit(&c->b, TH_POST, 2, "post B", 0) == 0);
        SimSpawned(&c->b);
        CHECK(pthread_create(&tb, NULL, WakerMain, c) == 0);
    }
    CHECK(pthread_create(&clock, NULL, SimClockMain, NULL) == 0);
    if (!withB) {
        SemWait(&h->idle);  // крючок ждёт resume: будим во время крючка
        SimWake(&c->a);
        SemPost(&h->resume);
        SemWait(&h->idle);  // крючок вернул 1, часы ждут: будим снаружи
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 30000000L};
        nanosleep(&ts, NULL);
        SimWake(&c->a);
    }
    pthread_join(clock, NULL);
    pthread_join(ta, NULL);
    SimThreadDestroy(&c->a);
    if (withB) {
        pthread_join(tb, NULL);
        SimThreadDestroy(&c->b);
    }
    CHECK(SemOpenCount() == sems);
}

static void TestParkWakeDoesNotAdvanceTime(void) {
    PairCtx c = {.aSaw = -1};
    HookLog h = {0};
    RunPair(&c, &h, 1);
    CHECK(c.aStatus == SIM_OK);
    CHECK(c.aSaw == 2);
    CHECK(c.extraWakes == 0 && c.again == SIM_STOP);
    CHECK(h.calls == 2);  // 0 (A ждёт, B спит), 2 (A снова ждёт) - и конец
    CHECK(h.now[0] == 0 && h.next[0] == 2);
    CHECK(h.now[1] == 2 && h.next[1] == -1);
    CHECK(SimTotalParks() == 2);
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

static void TestStopWakesParked(void) {
    PairCtx c = {.aSaw = -1};
    HookLog h = {.idleRounds = 2, .handshake = 1};
    CHECK(SemOpen(&h.idle, 0) == 0);
    CHECK(SemOpen(&h.resume, 0) == 0);
    RunPair(&c, &h, 0);
    SemClose(&h.idle);
    SemClose(&h.resume);
    CHECK(c.aStatus == SIM_OK);       // первую парковку сняли во время крючка
    CHECK(c.aSaw == 0);               // время при этом не шло
    CHECK(c.extraWakes == 1);         // вторую - когда часы ждали
    CHECK(c.again == SIM_STOP);       // третью - остановка модели
    CHECK(c.sleepAgain == SIM_STOP);  // после остановки - сразу SIM_STOP
    CHECK(SimStopping());
    CHECK(h.calls == 3);
    for (int i = 0; i < 3; ++i) {
        CHECK(h.now[i] == 0 && h.next[i] == -1);
    }
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

static void TestNoLocksWhenParking(void) {
    Mutex m;
    MutexInit(&m, RANK_STAGE, "stage test");
    PairCtx c = {.aSaw = -1, .hold = &m};
    HookLog h = {0};
    long before = SoftAssertFailures();
    RunPair(&c, &h, 1);
    CHECK(SoftAssertFailures() == before + 1);
    CHECK(c.aStatus == SIM_OK && c.aSaw == 2);
    MutexDestroy(&m);
}

typedef struct {
    SimThread a, b;
    _Atomic int woken;  // B уже вызвал SimWake(A)
    SimStatus aStatus;
    int aSaw;
} EarlyCtx;

static void *EarlyParker(void *arg) {
    EarlyCtx *c = arg;
    if (SimBegin(&c->a) == SIM_OK) {
        while (!atomic_load(&c->woken)) {
            // A работает: B будит его раньше, чем он запаркуется
        }
        c->aStatus = SimPark(&c->a, (WaitReason){.kind = WAIT_QUEUE});
        c->aSaw = SimNow();
    }
    SimExit(&c->a);
    return NULL;
}

static void *EarlyWaker(void *arg) {
    EarlyCtx *c = arg;
    if (SimBegin(&c->b) == SIM_OK) {
        SimWake(&c->a);
        atomic_store(&c->woken, 1);
    }
    SimExit(&c->b);
    return NULL;
}

/// Пробуждение работающего потока не теряется: его SimPark проходит сразу, время стоит.
static void TestWakeBeforePark(void) {
    EarlyCtx c = {.aStatus = SIM_STOP, .aSaw = -1};
    HookLog h = {0};
    SimOptions o = {.deterministic = 0, .delayMs = 0, .hook = RecordHook, .ctx = &h};
    long sems = FreshSim(&o, 0);
    pthread_t ta, tb, clock;
    CHECK(SimThreadInit(&c.a, TH_CAR, 1, "car A", 0) == 0);
    CHECK(SimThreadInit(&c.b, TH_CAR, 2, "car B", 0) == 0);
    SimSpawned(&c.a);
    SimSpawned(&c.b);
    CHECK(pthread_create(&ta, NULL, EarlyParker, &c) == 0);
    CHECK(pthread_create(&tb, NULL, EarlyWaker, &c) == 0);
    CHECK(pthread_create(&clock, NULL, SimClockMain, NULL) == 0);
    pthread_join(clock, NULL);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    SimThreadDestroy(&c.a);
    SimThreadDestroy(&c.b);
    CHECK(SemOpenCount() == sems);
    CHECK(c.aStatus == SIM_OK);
    CHECK(c.aSaw == 0);
    CHECK(h.calls == 1);
}

typedef struct {
    SimThread a, b;
    _Atomic int reasonSet;  // B уже вызвал SimSetReason(A)
    int alsoWake;           // B после причины ещё и будит A, пока тот работает
    SimStatus firstPark;    // при alsoWake: первая парковка A (должна пройти сразу)
    WaitReason seen;        // причина A, которую увидел крючок в минуту 0
    int seenState;
} ReasonCtx;

static void *ReasonParker(void *arg) {
    ReasonCtx *c = arg;
    if (SimBegin(&c->a) == SIM_OK) {
        while (!atomic_load(&c->reasonSet)) {
            // A ещё работает, когда другой поток задаёт ему причину
        }
        if (c->alsoWake) {
            c->firstPark = SimPark(&c->a, (WaitReason){.kind = WAIT_OPERATION});
            SimPark(&c->a, (WaitReason){.kind = WAIT_QUEUE, .stage = 7, .post = -1});
        } else {
            SimPark(&c->a, (WaitReason){.kind = WAIT_QUEUE, .stage = 0, .post = -1});
        }
    }
    SimExit(&c->a);
    return NULL;
}

static void *ReasonSetter(void *arg) {
    ReasonCtx *c = arg;
    if (SimBegin(&c->b) == SIM_OK) {
        SimSetReason(&c->a, (WaitReason){.kind = WAIT_OPERATION, .stage = 3, .post = 1});
        if (c->alsoWake) {
            SimWake(&c->a);
        }
        atomic_store(&c->reasonSet, 1);
        if (SimSleep(&c->b, 1) == SIM_OK) {
            SimWake(&c->a);
        }
    }
    SimExit(&c->b);
    return NULL;
}

static void LookAtA(const SimThread *t, void *ctx) {
    ReasonCtx *c = ctx;
    if (t == &c->a) {
        c->seen = t->why;
        c->seenState = (int)t->state;
    }
}

static int ReasonHook(int now, int nextWake, void *ctx) {
    if (now == 0) {
        SimForEach(LookAtA, ctx);
    }
    return nextWake != -1;
}

static void RunReason(ReasonCtx *c) {
    SimOptions o = {.deterministic = 0, .delayMs = 0, .hook = ReasonHook, .ctx = c};
    long sems = FreshSim(&o, 0);
    pthread_t ta, tb, clock;
    CHECK(SimThreadInit(&c->a, TH_CAR, 1, "car A", 0) == 0);
    CHECK(SimThreadInit(&c->b, TH_POST, 2, "post B", 0) == 0);
    SimSpawned(&c->a);
    SimSpawned(&c->b);
    CHECK(pthread_create(&ta, NULL, ReasonParker, c) == 0);
    CHECK(pthread_create(&tb, NULL, ReasonSetter, c) == 0);
    CHECK(pthread_create(&clock, NULL, SimClockMain, NULL) == 0);
    pthread_join(clock, NULL);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    SimThreadDestroy(&c->a);
    SimThreadDestroy(&c->b);
    CHECK(SemOpenCount() == sems);
}

/// SimSetReason работающему потоку не затирается аргументом его следующего SimPark.
static void TestReasonBeforePark(void) {
    ReasonCtx c = {.seenState = -1};
    RunReason(&c);
    CHECK(c.seenState == ST_PARKED);
    CHECK(c.seen.kind == WAIT_OPERATION && c.seen.stage == 3 && c.seen.post == 1);
}

/// Причина, заданная перед пробуждением, не переживает ожидание, которое прошло сразу.
static void TestStaleReasonDropped(void) {
    ReasonCtx c = {.seenState = -1, .alsoWake = 1, .firstPark = SIM_STOP};
    RunReason(&c);
    CHECK(c.firstPark == SIM_OK);
    CHECK(c.seenState == ST_PARKED);
    CHECK(c.seen.kind == WAIT_QUEUE && c.seen.stage == 7);
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
    RUN(TestSleepersWakeOnTime);
    RUN(TestManySleepersParallel);
    RUN(TestParkWakeDoesNotAdvanceTime);
    RUN(TestHookSeesQuiescence);
    RUN(TestStopWakesParked);
    RUN(TestWakeBeforePark);
    RUN(TestReasonBeforePark);
    RUN(TestStaleReasonDropped);
    RUN(TestDeterministicOrder);
    RUN(TestNoLocksWhenParking);
    return UnitSummary();
}
