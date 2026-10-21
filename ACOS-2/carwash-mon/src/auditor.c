/**
 * @file auditor.c
 * @brief Аудитор: свой учёт линии только по событиям и проверки I1-I5 после каждого события.
 *
 * В отличие от ИЗ1 линию не осматриваем: её меняют другие потоки, а событие уже несёт всё
 * нужное. Всё состояние ниже трогает только AuditorListener под мьютексом шины (и
 * AuditorPrint/AuditorFree после сбора потоков), поэтому своих мьютексов у аудитора нет.
 */

#include "auditor.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "memory.h"

#define INVARIANTS  5
#define MAX_REPORTS 10  // больше сообщений всё равно никто читать не будет

/// Где машина по мнению аудитора.
typedef enum {
    AT_UNKNOWN,   ///< ещё не приезжала
    AT_ARRIVING,  ///< на въезде: приехала, въезд не решён
    AT_QUEUE,     ///< в очереди стадии stage
    AT_POST,      ///< на посту post стадии stage
    AT_TRANSIT,   ///< уехала с поста стадии stage (EV_LEAVE), EV_MOVE/EV_DONE ещё не было
    AT_MOVED,     ///< EV_MOVE на стадию stage, EV_ENTER/EV_QUEUE ещё не было
    AT_GONE,      ///< уехала с мойки или получила отказ
} Where;

typedef struct {
    Where where;
    int stage, post;
    int program;
    int steps;  ///< сколько стадий маршрута машина уже начала
} CarView;

static const Config *config;
static int errOut = 2;
static long checks = 0;
static long violations = 0;
static int reports = 0;
static long perInvariant[INVARIANTS + 1];
static char printed[MAX_REPORTS][200];

static CarView *cars = NULL;
static int carCap = 0;
static int occupied[MAX_STAGES][MAX_POSTS];  ///< машин на посту
static int queued[MAX_STAGES];               ///< машин в очереди
static int broken[MAX_STAGES][MAX_POSTS];    ///< между EV_FAIL и EV_REPAIR

static const char *invariantName[INVARIANTS + 1] = {
    "",
    "a post holds at most its capacity",
    "a car is in exactly one place",
    "stages follow the program order",
    "a broken post starts no operation",
    "queue capacity is not exceeded",
};

void AuditorInit(int errFd, const Config *cfg) {
    AuditorFree();
    errOut = errFd;
    config = cfg;
    checks = violations = 0;
    reports = 0;
    memset(perInvariant, 0, sizeof(perInvariant));
    memset(occupied, 0, sizeof(occupied));
    memset(queued, 0, sizeof(queued));
    memset(broken, 0, sizeof(broken));
}

/// Учёт машины по номеру; массив растёт по мере приезда.
static CarView *View(int car) {
    if (car >= carCap) {
        int newCap = carCap ? carCap : 64;
        while (newCap <= car) {
            newCap *= 2;
        }
        cars = CheckedRealloc(cars, (size_t)newCap * sizeof *cars);
        for (int i = carCap; i < newCap; ++i) {
            cars[i] = (CarView){.where = AT_UNKNOWN, .stage = -1, .post = -1, .program = -1};
        }
        carCap = newCap;
    }
    return &cars[car];
}

/// Пока ошибка не исправлена, она находится после каждого события.
/// Считаем все случаи, а печатаем каждое сообщение только один раз.
static void Violation(int inv, const Event *e, const char *fmt, ...) {
    char text[200], clock[16], line[400];
    ++violations;
    ++perInvariant[inv];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    for (int i = 0; i < reports; ++i) {
        if (strcmp(text, printed[i]) == 0) {
            return;
        }
    }
    if (reports == MAX_REPORTS) {
        return;
    }
    snprintf(printed[reports++], sizeof(printed[0]), "%s", text);
    EventClock(e->time, clock, sizeof(clock));
    // одним write: в stderr пишут и другие потоки (SOFT_ASSERT, LOCKDEP)
    int n = snprintf(line, sizeof(line), "%s [%s] I%d violated (%s): %s\n",
                     isatty(errOut) ? "\033[1;41m AUDITOR \033[0m" : "AUDITOR:", clock, inv,
                     invariantName[inv], text);
    if (n > (int)sizeof(line) - 1) {
        n = (int)sizeof(line) - 1;
    }
    if (n > 0 && write(errOut, line, (size_t)n) < 0) {
        // stderr закрыт: нарушение всё равно посчитано и даст код 4
    }
}

static const char *StageName(int s) {
    return s >= 0 && s < config->stages ? config->stage[s].name : "?";
}

/// Где машина, словами: "in wax queue", "on tunnel post 2".
static void Describe(const CarView *c, char *buf, int size) {
    switch (c->where) {
    case AT_UNKNOWN: snprintf(buf, size, "not arrived"); break;
    case AT_ARRIVING: snprintf(buf, size, "at the entrance"); break;
    case AT_QUEUE: snprintf(buf, size, "in %s queue", StageName(c->stage)); break;
    case AT_POST: snprintf(buf, size, "on %s post %d", StageName(c->stage), c->post + 1); break;
    case AT_TRANSIT: snprintf(buf, size, "on the way from %s", StageName(c->stage)); break;
    case AT_MOVED: snprintf(buf, size, "moving to %s", StageName(c->stage)); break;
    case AT_GONE: snprintf(buf, size, "gone"); break;
    }
}

/// I3: машина начинает стадию s - она должна быть следующей по маршруту.
static void BeginStage(const Event *e, CarView *c, int s) {
    if (c->program < 0 || c->program >= config->programs) {
        return;  // программа неизвестна: сверять не с чем
    }
    const ProgramSpec *p = &config->program[c->program];
    if (c->steps >= p->len || p->route[c->steps] != s) {
        Violation(3, e, "car #%d entered %s out of %s order", e->car + 1, StageName(s), p->name);
    }
    ++c->steps;
}

/// I2 (и I3 на въезде): машина появляется на стадии s - в очереди или прямо на посту. Можно
/// только с въезда или после EV_MOVE на эту же стадию.
static void Arrive(const Event *e, CarView *c, int s) {
    if (c->where == AT_ARRIVING) {
        BeginStage(e, c, s);
    } else if (c->where != AT_MOVED || c->stage != s) {
        char where[64];
        Describe(c, where, sizeof(where));
        Violation(2, e, "car #%d appears at %s while %s", e->car + 1, StageName(s), where);
    }
}

static void OnQueue(const Event *e, CarView *c) {
    int s = e->stage;
    Arrive(e, c, s);
    if (c->where == AT_QUEUE) {
        --queued[c->stage];  // уже стояла в другой очереди (нарушение I2 выше)
    }
    ++queued[s];
    if (queued[s] > config->stage[s].queue) {
        Violation(5, e, "%s queue holds %d cars, capacity is %d", StageName(s), queued[s],
                  config->stage[s].queue);
    }
    *c = (CarView){
        .where = AT_QUEUE, .stage = s, .post = -1, .program = c->program, .steps = c->steps};
}

static void OnEnter(const Event *e, CarView *c) {
    int s = e->stage, p = e->post;
    if (c->where == AT_QUEUE && c->stage == s) {
        --queued[s];  // из очереди на пост той же стадии
    } else {
        Arrive(e, c, s);
    }
    if (p >= config->stage[s].posts) {
        Violation(1, e, "non-existent %s post %d holds car #%d", StageName(s), p + 1, e->car + 1);
    } else if (++occupied[s][p] > config->stage[s].capacity) {
        Violation(1, e, "%s post %d holds %d cars, capacity is %d", StageName(s), p + 1,
                  occupied[s][p], config->stage[s].capacity);
    }
    *c = (CarView){
        .where = AT_POST, .stage = s, .post = p, .program = c->program, .steps = c->steps};
}

static void OnLeave(const Event *e, CarView *c) {
    int s = e->stage, p = e->post;
    if (c->where == AT_POST && c->stage == s && c->post == p) {
        --occupied[s][p];
    } else {
        char where[64];
        Describe(c, where, sizeof(where));
        Violation(2, e, "car #%d leaves %s post %d while %s", e->car + 1, StageName(s), p + 1,
                  where);
    }
    *c = (CarView){
        .where = AT_TRANSIT, .stage = s, .post = -1, .program = c->program, .steps = c->steps};
}

static void OnMove(const Event *e, CarView *c) {
    if (c->where != AT_TRANSIT || c->stage != e->arg) {
        char where[64];
        Describe(c, where, sizeof(where));
        Violation(2, e, "car #%d moves from %s while %s", e->car + 1, StageName(e->arg), where);
    }
    BeginStage(e, c, e->stage);
    *c = (CarView){
        .where = AT_MOVED, .stage = e->stage, .post = -1, .program = c->program, .steps = c->steps};
}

static void OnDone(const Event *e, CarView *c) {
    if (c->where != AT_TRANSIT) {
        char where[64];
        Describe(c, where, sizeof(where));
        Violation(2, e, "car #%d leaves the wash while %s", e->car + 1, where);
    }
    if (c->program >= 0 && c->program < config->programs &&
        c->steps != config->program[c->program].len) {
        Violation(3, e, "car #%d left after %d of %d stages", e->car + 1, c->steps,
                  config->program[c->program].len);
    }
    c->where = AT_GONE;
}

void AuditorListener(const Event *e) {
    ++checks;
    CarView *c = e->car >= 0 ? View(e->car) : NULL;
    if (c && e->program >= 0 && c->program < 0) {
        c->program = e->program;
    }
    if (c == NULL && e->type <= EV_DONE && e->type != EV_FAIL && e->type != EV_REPAIR) {
        return;  // событию машины нужен её номер; без него проверять нечего
    }
    switch (e->type) {
    case EV_ARRIVE:
        if (c->where != AT_UNKNOWN) {
            Violation(2, e, "car #%d arrives twice", e->car + 1);
        }
        c->where = AT_ARRIVING;
        break;
    case EV_PROGRAM: c->program = e->arg; break;
    case EV_REJECT:
        if (c->where != AT_ARRIVING) {
            Violation(2, e, "car #%d is turned away but is not at the entrance", e->car + 1);
        }
        c->where = AT_GONE;
        break;
    case EV_QUEUE: OnQueue(e, c); break;
    case EV_ENTER: OnEnter(e, c); break;
    case EV_OP_START:
        if (broken[e->stage][e->post]) {
            Violation(4, e, "%s post %d started car #%d while broken", StageName(e->stage),
                      e->post + 1, e->car + 1);
        }
        if (c->where != AT_POST || c->stage != e->stage || c->post != e->post) {
            Violation(2, e, "%s post %d started car #%d that is not on it", StageName(e->stage),
                      e->post + 1, e->car + 1);
        }
        break;
    case EV_LEAVE: OnLeave(e, c); break;
    case EV_MOVE: OnMove(e, c); break;
    case EV_DONE: OnDone(e, c); break;
    case EV_FAIL: broken[e->stage][e->post] = 1; break;
    case EV_REPAIR: broken[e->stage][e->post] = 0; break;
    default: break;
    }
}

long AuditorChecks(void) {
    return checks;
}

long AuditorViolations(void) {
    return violations;
}

void AuditorPrint(int fd) {
    dprintf(fd, "AUDITOR: invariants checked after each of %ld events, violations: %ld\n", checks,
            violations);
    for (int i = 1; i <= INVARIANTS; ++i) {
        dprintf(fd, "  I%d %-36s %s", i, invariantName[i], perInvariant[i] ? "FAILED" : "ok");
        if (perInvariant[i]) {
            dprintf(fd, " (%ld times)", perInvariant[i]);
        }
        dprintf(fd, "\n");
    }
}

void AuditorFree(void) {
    free(cars);
    cars = NULL;
    carCap = 0;
}
