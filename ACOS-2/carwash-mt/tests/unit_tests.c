#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "auditor.h"
#include "config.h"
#include "events.h"
#include "line.h"
#include "memory.h"
#include "mutex.h"
#include "render.h"
#include "rng.h"
#include "sem.h"
#include "sim.h"
#include "snapshot.h"
#include "soft_assert.h"
#include "stage.h"
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

/// Крючок для SimKick: считает вызовы, о каждом сообщает через idle, на stopAt-м останавливает.
typedef struct {
    int calls, stopAt;
    int now[4];
    Sem idle;
} KickLog;

static int KickHook(int now, int nextWake, void *ctx) {
    (void)nextWake;
    KickLog *k = ctx;
    if (k->calls < 4) {
        k->now[k->calls] = now;
    }
    int go = ++k->calls < k->stopAt;
    SemPost(&k->idle);
    return go;
}

static long MsSince(const struct timespec *t0) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return ((t.tv_sec - t0->tv_sec) * 1000L) + ((t.tv_nsec - t0->tv_nsec) / 1000000L);
}

/// Часы ждут без спящих (nextWake == -1): SimKick заставляет их снова вызвать крючок.
static void TestKickRerunsHook(void) {
    KickLog k = {.stopAt = 3};
    CHECK(SemOpen(&k.idle, 0) == 0);
    SimOptions o = {.delayMs = 0, .hook = KickHook, .ctx = &k};
    FreshSim(&o, 1);
    pthread_t clock;
    CHECK(pthread_create(&clock, NULL, SimClockMain, NULL) == 0);
    SemWait(&k.idle);  // первый покой: потоков нет, крючок вернул 1, часы ждут
    SimKick();
    SemWait(&k.idle);
    SimKick();  // третий вызов останавливает модель
    pthread_join(clock, NULL);
    CHECK(k.calls == 3);
    CHECK(k.now[0] == 0 && k.now[1] == 0 && k.now[2] == 0);
    SemClose(&k.idle);
}

/// SimKick во время задержки темпа: крючок вызывается сразу, в ту же минуту, а не через -d.
static void TestKickCutsDelay(void) {
    KickLog k = {.stopAt = 2};
    CHECK(SemOpen(&k.idle, 0) == 0);
    SimOptions o = {.delayMs = 5000, .hook = KickHook, .ctx = &k};
    FreshSim(&o, 1);
    Worker w = {.th = {.id = 0}, .step = 1, .count = 8};
    pthread_t tid, clock;
    CHECK(SimThreadInit(&w.th, TH_POST, 0, "post", 0) == 0);
    SimSpawned(&w.th);
    CHECK(pthread_create(&tid, NULL, WorkerMain, &w) == 0);
    CHECK(pthread_create(&clock, NULL, SimClockMain, NULL) == 0);
    SemWait(&k.idle);  // крючок вернул 1, часы в задержке на 5 с
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    SimKick();
    pthread_join(clock, NULL);
    CHECK(MsSince(&t0) < 1000);
    CHECK(k.calls == 2 && k.now[1] == k.now[0]);
    pthread_join(tid, NULL);
    CHECK(w.last == SIM_STOP);
    SimThreadDestroy(&w.th);
    SemClose(&k.idle);
}

/// SimShutdown закрывает семафор часов; повторный вызов ничего не делает.
static void TestShutdownClosesClock(void) {
    SimOptions o = {0};
    long open = FreshSim(&o, 0);
    SimShutdown();
    CHECK(SemOpenCount() == open - 1);
    SimShutdown();
    CHECK(SemOpenCount() == open - 1);
    SimKick();  // после SimShutdown безопасен
    FreshSim(&o, 0);
}

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
        CHECK(pthread_create(&th[i], NULL, EmitterMain, &ids[i]) == 0);
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

// ---- стадии: бронь, переезд, эстафета (однопоточно: машины - записи без потоков) ----

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
static Event evLog[EV_LOG_MAX];  // слушатель пишет в том же потоке, что и тест
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

/// Свежая модель, шина с записью событий и линия по lineCfg.
static Line *OpenLine(void) {
    SimOptions o = {0};
    CHECK(SimInit(&o) == 0);
    EventsInit(&lineCfg);
    EventsSubscribe(RecordListener);
    evCount = 0;
    CHECK(LineInit(&line, &lineCfg) == 0);
    return &line;
}

/// Машина без потока (только семафор), в реестре линии; LineFree её освободит.
static Car *NewCar(Line *l, int program) {
    Car *car = calloc(1, sizeof *car);
    if (car == NULL) {
        abort();
    }
    MutexLock(&l->registry);
    if (l->carCount == l->carCap) {
        l->carCap = l->carCap ? l->carCap * 2 : 8;
        l->cars = (Car **)CheckedRealloc((void *)l->cars, (size_t)l->carCap * sizeof *l->cars);
    }
    car->id = l->carCount;
    l->cars[l->carCount++] = car;
    MutexUnlock(&l->registry);
    char name[24];
    snprintf(name, sizeof name, "car #%d", car->id + 1);
    CHECK(SimThreadInit(&car->th, TH_CAR, car->id, name, 0) == 0);
    car->program = program;
    car->place = CAR_ARRIVING;
    car->stage = car->post = car->seat = -1;
    car->res = RES_NONE;
    car->resStage = car->resPost = car->resSeat = -1;
    car->ticket = -1;
    car->finished = -1;
    return car;
}

/// StageAdmit на стадии s и, если место дали и @p commit, сразу StageCommit с въезда.
static Admit Enter(Line *l, int s, Car *car, int mayWait, int commit) {
    Stage *st = &l->stage[s];
    MutexLock(&st->m);
    Admit a = StageAdmit(l, st, car, mayWait);
    if (commit && a != ADMIT_FULL) {
        StageCommit(l, st, car, -1);
    }
    MutexUnlock(&st->m);
    return a;
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

static void Dispatch(Line *l, int s) {
    Stage *st = &l->stage[s];
    MutexLock(&st->m);
    StageDispatch(l, st);
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

static void TestAdmitDirectSeat(void) {
    LoadLineCfg(NULL);
    Line *l = OpenLine();
    Car *car = NewCar(l, P_EXPRESS);
    Stage *st = &l->stage[S_TUNNEL];
    MutexLock(&st->m);
    CHECK(StageAdmit(l, st, car, 0) == ADMIT_SEAT);
    CHECK(car->res == RES_SEAT && car->resStage == S_TUNNEL && car->resPost == 0);
    CHECK(st->post[0].reserved == 1);
    StageCommit(l, st, car, -1);
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
        CHECK(Enter(l, S_TUNNEL, cars[i], 0, 1) == want[i]);
    }
    CHECK(cars[0]->post == 0 && cars[1]->post == 1);
    CHECK(cars[2]->place == CAR_QUEUE && cars[3]->place == CAR_QUEUE);
    CHECK(cars[4]->place == CAR_ARRIVING && cars[4]->ticket == -1);
    CHECK(Queued(l, S_TUNNEL) == 2 && Waiters(l, S_TUNNEL) == 0);
    int q = FindEv(EV_QUEUE, cars[3]->id);
    CHECK(q >= 0 && evLog[q].arg == 2);
    LineFree(l);
}

static void TestCapacityTwo(void) {
    LoadLineCfg(CAPACITY_CFG);
    Line *l = OpenLine();
    Car *a = NewCar(l, P_EXPRESS), *b = NewCar(l, P_EXPRESS), *c = NewCar(l, P_EXPRESS);
    CHECK(Enter(l, S_DRY, a, 0, 1) == ADMIT_SEAT);
    CHECK(Enter(l, S_DRY, b, 0, 1) == ADMIT_SEAT);
    CHECK(Enter(l, S_DRY, c, 0, 1) == ADMIT_QUEUE);
    CHECK(a->post == 0 && b->post == 0 && a->seat == 0 && b->seat == 1);
    CHECK(c->place == CAR_QUEUE);
    LineFree(l);
}

static void TestReservationCounts(void) {
    LoadLineCfg(ONE_CFG);
    Line *l = OpenLine();
    Car *a = NewCar(l, 0), *b = NewCar(l, 0), *c = NewCar(l, 0);
    Stage *st = &l->stage[0];
    MutexLock(&st->m);
    CHECK(StageAdmit(l, st, a, 0) == ADMIT_SEAT);   // бронь места, без Commit
    CHECK(StageAdmit(l, st, b, 0) == ADMIT_QUEUE);  // место занято бронью
    CHECK(st->queueReserved == 1 && st->queued == 0);
    CHECK(StageAdmit(l, st, c, 0) == ADMIT_FULL);  // бронь очереди учтена во вместимости
    CHECK(StageAdmit(l, st, a, 0) == ADMIT_SEAT);  // повторный вызов возвращает ту же бронь
    CHECK(st->post[0].reserved == 1);
    StageCommit(l, st, a, -1);
    StageCommit(l, st, b, -1);
    CHECK(st->queueReserved == 0 && st->queued == 1 && b->place == CAR_QUEUE);
    CHECK(StageAdmit(l, st, c, 0) == ADMIT_FULL);
    MutexUnlock(&st->m);
    LineFree(l);

    // место освободилось, пока машина с бронью очереди ещё в пути: новая машина его не займёт
    LoadLineCfg(NULL);
    l = OpenLine();
    st = &l->stage[S_TUNNEL];
    Car *t1 = NewCar(l, P_EXPRESS), *t2 = NewCar(l, P_EXPRESS);
    Car *q = NewCar(l, P_EXPRESS), *n = NewCar(l, P_EXPRESS);
    Enter(l, S_TUNNEL, t1, 0, 1);
    Enter(l, S_TUNNEL, t2, 0, 1);
    CHECK(Enter(l, S_TUNNEL, q, 0, 0) == ADMIT_QUEUE);  // бронь очереди, без Commit
    Vacate(l, S_TUNNEL, t1);
    CHECK(Enter(l, S_TUNNEL, n, 0, 0) == ADMIT_QUEUE);  // не SEAT: очередь не пуста
    MutexLock(&st->m);
    StageCommit(l, st, q, S_PREWASH);  // въехал в очередь - эстафета сразу сажает его
    CHECK(q->place == CAR_POST && q->post == 0);
    StageCommit(l, st, n, S_PREWASH);
    CHECK(n->place == CAR_QUEUE);
    MutexUnlock(&st->m);
    int qq = FindEv(EV_QUEUE, q->id);
    CHECK(qq >= 0 && FindEv(EV_ENTER, q->id) == qq + 1);
    LineFree(l);
}

static void TestPriorityPop(void) {
    for (int fifo = 0; fifo <= 1; ++fifo) {
        Config *cfg = LoadLineCfg(PRIORITY_CFG);
        cfg->rule = fifo ? PRIO_FIFO : PRIO_PROGRAM;
        Line *l = OpenLine();
        Car *a = NewCar(l, P_STANDARD), *express = NewCar(l, P_EXPRESS);
        Car *premium = NewCar(l, P_PREMIUM);
        CHECK(Enter(l, S_TUNNEL, a, 0, 1) == ADMIT_SEAT);
        CHECK(Enter(l, S_TUNNEL, express, 0, 1) == ADMIT_QUEUE);
        CHECK(Enter(l, S_TUNNEL, premium, 0, 1) == ADMIT_QUEUE);
        Vacate(l, S_TUNNEL, a);
        Car *won = fifo ? express : premium, *lost = fifo ? premium : express;
        CHECK(won->place == CAR_POST && won->post == 0 && lost->place == CAR_QUEUE);
        CHECK(won->th.why.kind == WAIT_OPERATION && won->th.why.stage == S_TUNNEL);
        CHECK(Queued(l, S_TUNNEL) == 1);
        int leave = FindEv(EV_LEAVE, a->id), enter = FindEv(EV_ENTER, won->id);
        CHECK(leave >= 0 && enter > leave && FindEv(EV_OP_START, won->id) == enter + 1);
        LineFree(l);
    }
}

static void SetTicket(Line *l, int s, Car *car, long ticket) {
    MutexLock(&l->stage[s].m);
    car->ticket = ticket;
    MutexUnlock(&l->stage[s].m);
}

/// Тоннель zero-buffer занят двумя машинами, ждущие места - по порядку номеров в списке.
static void TestWaitersOrder(void) {
    // ticket 4 (Standard) раньше ticket 5 (Express)
    Config *cfg = LoadLineCfg(ZERO_CFG);
    cfg->rule = PRIO_FIFO;
    Line *l = OpenLine();
    Car *t1 = NewCar(l, P_EXPRESS), *t2 = NewCar(l, P_EXPRESS);
    CHECK(Enter(l, S_TUNNEL, t1, 0, 1) == ADMIT_SEAT && Enter(l, S_TUNNEL, t2, 0, 1) == ADMIT_SEAT);
    MutexLock(&l->counters);
    l->nextTicket = 3;
    MutexUnlock(&l->counters);
    Car *standard = NewCar(l, P_STANDARD), *express = NewCar(l, P_EXPRESS);
    CHECK(Enter(l, S_TUNNEL, express, 1, 0) == ADMIT_FULL);
    CHECK(Enter(l, S_TUNNEL, standard, 1, 0) == ADMIT_FULL);
    CHECK(express->ticket == 4 && standard->ticket == 5);
    SetTicket(l, S_TUNNEL, express, 5);  // в списке Express стоит первым, но номер у него больше
    SetTicket(l, S_TUNNEL, standard, 4);
    Vacate(l, S_TUNNEL, t1);
    CHECK(standard->res == RES_SEAT && standard->ticket == -1);
    CHECK(express->res == RES_NONE && express->ticket == 5 && Waiters(l, S_TUNNEL) == 1);
    LineFree(l);

    // номер в списке важнее приоритета: Express (раньше) получает место раньше Premium
    cfg = LoadLineCfg(ZERO_CFG);
    cfg->rule = PRIO_PROGRAM;
    l = OpenLine();
    t1 = NewCar(l, P_EXPRESS);
    t2 = NewCar(l, P_EXPRESS);
    Enter(l, S_TUNNEL, t1, 0, 1);
    Enter(l, S_TUNNEL, t2, 0, 1);
    express = NewCar(l, P_EXPRESS);
    Car *premium = NewCar(l, P_PREMIUM);
    CHECK(Enter(l, S_TUNNEL, express, 1, 0) == ADMIT_FULL);
    CHECK(Enter(l, S_TUNNEL, premium, 1, 0) == ADMIT_FULL);
    Vacate(l, S_TUNNEL, t1);
    CHECK(express->res == RES_SEAT && premium->res == RES_NONE);
    LineFree(l);

    // равные номера: выше приоритет, затем меньший номер машины
    cfg = LoadLineCfg(ZERO_CFG);
    cfg->rule = PRIO_PROGRAM;
    l = OpenLine();
    t1 = NewCar(l, P_EXPRESS);
    t2 = NewCar(l, P_EXPRESS);
    Enter(l, S_TUNNEL, t1, 0, 1);
    Enter(l, S_TUNNEL, t2, 0, 1);
    express = NewCar(l, P_EXPRESS);
    Car *p1 = NewCar(l, P_PREMIUM), *p2 = NewCar(l, P_PREMIUM);
    CHECK(Enter(l, S_TUNNEL, p2, 1, 0) == ADMIT_FULL);  // в список в обратном порядке
    CHECK(Enter(l, S_TUNNEL, p1, 1, 0) == ADMIT_FULL);
    CHECK(Enter(l, S_TUNNEL, express, 1, 0) == ADMIT_FULL);
    SetTicket(l, S_TUNNEL, express, 7);
    SetTicket(l, S_TUNNEL, p1, 7);
    SetTicket(l, S_TUNNEL, p2, 7);
    Vacate(l, S_TUNNEL, t1);
    CHECK(p1->res == RES_SEAT && p2->res == RES_NONE && express->res == RES_NONE);
    Vacate(l, S_TUNNEL, t2);
    CHECK(p2->res == RES_SEAT && express->res == RES_NONE && Waiters(l, S_TUNNEL) == 1);
    LineFree(l);
}

static void TestBrokenPostTakesNobody(void) {
    LoadLineCfg(NULL);
    Line *l = OpenLine();
    SetRepair(l, S_TUNNEL, 0, 5);
    Car *a = NewCar(l, P_EXPRESS), *b = NewCar(l, P_EXPRESS);
    CHECK(Enter(l, S_TUNNEL, a, 0, 0) == ADMIT_SEAT);
    CHECK(a->resPost == 1);  // не на сломанный пост 0
    CHECK(Enter(l, S_TUNNEL, b, 0, 0) == ADMIT_QUEUE);
    Stage *st = &l->stage[S_TUNNEL];
    MutexLock(&st->m);
    StageCommit(l, st, a, -1);
    StageCommit(l, st, b, -1);  // Commit зовёт Dispatch: на сломанный пост не сажает
    MutexUnlock(&st->m);
    CHECK(b->place == CAR_QUEUE);
    Dispatch(l, S_TUNNEL);
    CHECK(b->place == CAR_QUEUE && FindEv(EV_ENTER, b->id) == -1);
    SetRepair(l, S_TUNNEL, 0, 0);  // починили - эстафета сажает
    Dispatch(l, S_TUNNEL);
    CHECK(b->place == CAR_POST && b->post == 0 && Queued(l, S_TUNNEL) == 0);
    LineFree(l);
}

static void TestZeroQueue(void) {
    LoadLineCfg(ZERO_CFG);
    Line *l = OpenLine();
    Car *a = NewCar(l, P_EXPRESS), *b = NewCar(l, P_EXPRESS), *c = NewCar(l, P_EXPRESS);
    CHECK(Enter(l, S_TUNNEL, a, 0, 1) == ADMIT_SEAT);
    CHECK(Enter(l, S_TUNNEL, b, 0, 1) == ADMIT_SEAT);
    CHECK(Enter(l, S_TUNNEL, c, 0, 0) == ADMIT_FULL && Waiters(l, S_TUNNEL) == 0);
    CHECK(Enter(l, S_TUNNEL, c, 1, 0) == ADMIT_FULL && Waiters(l, S_TUNNEL) == 1);
    long ticket = c->ticket;
    CHECK(ticket >= 1);
    CHECK(Enter(l, S_TUNNEL, c, 1, 0) == ADMIT_FULL);  // повторно в список не встаёт
    CHECK(Waiters(l, S_TUNNEL) == 1 && c->ticket == ticket);
    Vacate(l, S_TUNNEL, b);
    CHECK(FindEv(EV_LEAVE, b->id) >= 0);
    CHECK(c->res == RES_SEAT && c->resPost == 1 && c->ticket == -1 && Waiters(l, S_TUNNEL) == 0);
    Stage *st = &l->stage[S_TUNNEL];
    MutexLock(&st->m);
    CHECK(StageAdmit(l, st, c, 1) == ADMIT_SEAT);  // машина проснулась и спрашивает снова
    StageCommit(l, st, c, S_PREWASH);
    MutexUnlock(&st->m);
    int move = FindEv(EV_MOVE, c->id);
    CHECK(move >= 0 && evLog[move].stage == S_TUNNEL && evLog[move].arg == S_PREWASH);
    CHECK(FindEv(EV_ENTER, c->id) == move + 1 && c->post == 1);
    LineFree(l);

    // место появилось без эстафеты (пост починился, Dispatch ещё не звали): новая машина не
    // обгоняет ожидающую
    LoadLineCfg(ZERO_CFG);
    l = OpenLine();
    SetRepair(l, S_TUNNEL, 0, 5);
    Car *t = NewCar(l, P_EXPRESS), *w = NewCar(l, P_EXPRESS), *n = NewCar(l, P_EXPRESS);
    CHECK(Enter(l, S_TUNNEL, t, 0, 1) == ADMIT_SEAT && t->post == 1);
    CHECK(Enter(l, S_TUNNEL, w, 1, 0) == ADMIT_FULL);
    SetRepair(l, S_TUNNEL, 0, 0);
    CHECK(Enter(l, S_TUNNEL, n, 0, 0) == ADMIT_FULL);
    Dispatch(l, S_TUNNEL);
    CHECK(w->res == RES_SEAT && w->resPost == 0);
    LineFree(l);
}

static _Atomic int waiterDone;
static Admit waiterGot;  // пишет поток машины до waiterDone, читает тест после join

/// Поток машины: ждёт место в тоннеле, держа prewash, как будет в CarMain.
static void *WaiterMain(void *arg) {
    Car *car = arg;
    Stage *st = &line.stage[S_TUNNEL];
    if (SimBegin(&car->th) == SIM_OK) {
        MutexLock(&st->m);
        Admit a = StageAdmit(&line, st, car, 1);
        MutexUnlock(&st->m);
        while (a == ADMIT_FULL) {  // SimPark может вернуться зря: условие - в цикле
            WaitReason why = {.kind = WAIT_ROOM, .stage = S_TUNNEL, .post = -1};
            if (SimPark(&car->th, why) != SIM_OK) {
                break;
            }
            MutexLock(&st->m);
            a = StageAdmit(&line, st, car, 1);
            MutexUnlock(&st->m);
        }
        if (a != ADMIT_FULL) {
            MutexLock(&st->m);
            StageCommit(&line, st, car, S_PREWASH);
            MutexUnlock(&st->m);
        }
        waiterGot = a;
    }
    SimExit(&car->th);
    atomic_store(&waiterDone, 1);
    return NULL;
}

/// Ждёт флага до 2 с. @return 1, если дождался
static int WaitFlag(_Atomic int *flag) {
    for (int i = 0; i < 2000 && !atomic_load(flag); ++i) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L};
        nanosleep(&ts, NULL);
    }
    return atomic_load(flag);
}

/// Настоящий поток: освобождающий выдаёт бронь и будит припаркованную машину.
static void TestGrantWakesParkedCar(void) {
    LoadLineCfg(ZERO_CFG);
    Line *l = OpenLine();
    Car *t1 = NewCar(l, P_EXPRESS), *t2 = NewCar(l, P_EXPRESS), *w = NewCar(l, P_STANDARD);
    Enter(l, S_TUNNEL, t1, 0, 1);
    Enter(l, S_TUNNEL, t2, 0, 1);
    atomic_store(&waiterDone, 0);
    waiterGot = ADMIT_FULL;
    SimSpawned(&w->th);
    pthread_t th;
    CHECK(pthread_create(&th, NULL, WaiterMain, w) == 0);
    int listed = 0;
    for (int i = 0; i < 2000 && !(listed = Waiters(l, S_TUNNEL) == 1); ++i) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L};
        nanosleep(&ts, NULL);
    }
    CHECK(listed);
    Vacate(l, S_TUNNEL, t1);
    int woke = WaitFlag(&waiterDone);
    CHECK(woke);
    if (!woke) {
        SimStop();  // без пробуждения поток так и висел бы на семафоре
    }
    pthread_join(th, NULL);
    CHECK(waiterGot == ADMIT_SEAT);
    CHECK(w->place == CAR_POST && w->stage == S_TUNNEL && w->post == 0);
    LineFree(l);
}

static void TestSabotageOverflow(void) {
    Config *cfg = LoadLineCfg(ONE_CFG);
    cfg->sabotage = SAB_OVERFLOW;
    Line *l = OpenLine();
    Car *a = NewCar(l, 0), *b = NewCar(l, 0), *c = NewCar(l, 0), *d = NewCar(l, 0);
    CHECK(Enter(l, 0, a, 0, 1) == ADMIT_SEAT);
    CHECK(Enter(l, 0, b, 0, 1) == ADMIT_QUEUE);
    long before = SoftAssertFailures();
    CHECK(Enter(l, 0, c, 0, 1) == ADMIT_QUEUE);  // ошибка на единицу пускает лишнюю
    CHECK(SoftAssertFailures() > before);        // а Commit это замечает
    CHECK(Queued(l, 0) == 2);
    CHECK(Enter(l, 0, d, 0, 1) == ADMIT_FULL);
    LineFree(l);
}

/// Ожидающий стоит на посту prewash; место в тоннеле ему выдают под мьютексом тоннеля.
static void RunDeadlockScenario(Sabotage sab, long *violations) {
    Config *cfg = LoadLineCfg(ZERO_CFG);
    cfg->sabotage = sab;
    Line *l = OpenLine();
    Car *p = NewCar(l, P_STANDARD), *t1 = NewCar(l, P_EXPRESS), *t2 = NewCar(l, P_EXPRESS);
    CHECK(Enter(l, S_PREWASH, p, 0, 1) == ADMIT_SEAT);
    Enter(l, S_TUNNEL, t1, 0, 1);
    Enter(l, S_TUNNEL, t2, 0, 1);
    CHECK(Enter(l, S_TUNNEL, p, 1, 0) == ADMIT_FULL);
    long before = LockdepViolations();
    Vacate(l, S_TUNNEL, t1);
    CHECK(p->res == RES_SEAT);
    *violations = LockdepViolations() - before;
    CHECK(LockdepHeldCount() == 0);
    LineFree(l);
}

static void TestSabotageDeadlock(void) {
    LockdepSetFatal(0);
    long clean = -1, caught = -1;
    RunDeadlockScenario(SAB_NONE, &clean);
    RunDeadlockScenario(SAB_DEADLOCK, &caught);
    CHECK(clean == 0);
    CHECK(caught == 1);
    LockdepSetFatal(1);
}

/// Запросы момента покоя на dead-stage: машина в очереди wax, у которой нет постов.
static void TestStallQueries(void) {
    LoadLineCfg(DEAD_CFG);
    Line *l = OpenLine();
    Car *stuck = NewCar(l, P_PREMIUM);
    CHECK(Enter(l, S_WAX, stuck, 0, 1) == ADMIT_QUEUE);
    CHECK(LineProgressPossible(l) == 0);
    CHECK(LineStallCause(l) == S_WAX);
    CHECK(LineEntranceMayAccept(l) == 1);
    Car *busy = NewCar(l, P_EXPRESS);
    CHECK(Enter(l, S_TUNNEL, busy, 0, 1) == ADMIT_SEAT);
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
    CHECK(Enter(l, S_PREWASH, NewCar(l, P_STANDARD), 0, 1) == ADMIT_SEAT);
    CHECK(Enter(l, S_TUNNEL, NewCar(l, P_EXPRESS), 0, 1) == ADMIT_SEAT);
    CHECK(LineEntranceMayAccept(l) == 1);
    CHECK(Enter(l, S_TUNNEL, NewCar(l, P_EXPRESS), 0, 1) == ADMIT_SEAT);
    CHECK(LineEntranceMayAccept(l) == 0);
    CHECK(LineStallCause(l) == -1);
    LineFree(l);
}

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

/// Помытая машина на посту стадии s просит место на стадии next и встаёт в список ожидающих.
static void Block(Line *l, int s, Car *car, int next) {
    MutexLock(&l->stage[s].m);
    l->stage[s].post[car->post].seat[car->seat].left = 0;
    MutexUnlock(&l->stage[s].m);
    CHECK(Enter(l, next, car, 1, 0) == ADMIT_FULL);
}

/// Пример: wax без постов, перед ним всё забито.
static Line *StuckLine(void) {
    LoadLineCfg(DEAD_CFG);
    Line *l = OpenLine();
    CHECK(Enter(l, S_WAX, NewCar(l, P_PREMIUM), 0, 1) == ADMIT_QUEUE);  // #1
    CHECK(Enter(l, S_WAX, NewCar(l, P_PREMIUM), 0, 1) == ADMIT_QUEUE);  // #2
    Car *c3 = NewCar(l, P_PREMIUM), *c4 = NewCar(l, P_PREMIUM);
    CHECK(Enter(l, S_TUNNEL, c3, 0, 1) == ADMIT_SEAT);
    CHECK(Enter(l, S_TUNNEL, c4, 0, 1) == ADMIT_SEAT);
    Block(l, S_TUNNEL, c3, S_WAX);
    Block(l, S_TUNNEL, c4, S_WAX);
    Car *c5 = NewCar(l, P_STANDARD);
    CHECK(Enter(l, S_PREWASH, c5, 0, 1) == ADMIT_SEAT);
    CHECK(Enter(l, S_TUNNEL, NewCar(l, P_EXPRESS), 0, 1) == ADMIT_QUEUE);  // #6
    CHECK(Enter(l, S_TUNNEL, NewCar(l, P_EXPRESS), 0, 1) == ADMIT_QUEUE);  // #7
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
    CHECK(Enter(l, S_DRY, NewCar(l, P_EXPRESS), 0, 1) == ADMIT_QUEUE);
    const char *g = Capture(WaitGraphPrint, l);
    CHECK(strstr(g, "(1 car(s) stuck)"));
    CHECK(strstr(g, "  car #1   in dry queue       -> waits for a post in dry\n"));
    CHECK(strstr(g, "    dry: 2/2 posts broken       <- ROOT CAUSE: all dry posts are broken "
                    "(repair 4 min)\n"));
    LineFree(l);
}

static void TestWaitGraphDot(void) {
    Line *l = StuckLine();
    char path[] = "/tmp/cwmt-dot-XXXXXX";
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

// --- живая схема: панель потоков в снимке, кадр, поток отрисовки ----------------------------

/// Состояние потока машины, как будто она припаркована с такой причиной.
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
    long before = SemOpenCount();
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
    CHECK(SemOpenCount() == before);  // семафоры закрыты
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

/// Остановка без единого кадра: поток ждёт ready, RenderStop его будит.
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

// --- аудитор: события подаются прямо в AuditorListener, без шины ---------------------------

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
    // между LEAVE и MOVE машины 0 на освободившийся пост садится машина 1 (эстафета)
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
    RUN(TestKickRerunsHook);
    RUN(TestKickCutsDelay);
    RUN(TestShutdownClosesClock);
    RUN(TestDeterministicOrder);
    RUN(TestNoLocksWhenParking);
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
    RUN(TestGrantWakesParkedCar);
    RUN(TestStallQueries);
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
    return UnitSummary();
}
