#include "state.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

int StateInit(State *s, const Config *cfg) {
    memset(s, 0, sizeof(*s));
    s->cfg = *cfg;
    s->nextId = 1;
    for (int i = 0; i < MAX_SLOTS; ++i) {
        s->car[i].state = CAR_FREE;
        s->car[i].waitStage = -1;
    }
    for (int i = 0; i < MAX_STAGES; ++i) {
        for (int p = 0; p < MAX_POSTS; ++p) {
            s->post[i][p].state = POST_OFF;
            s->post[i][p].car = -1;
        }
    }
    // каждая машина на линии держит место в накопителе или пост; плюс те, что стоят у въезда
    int capacity = 0;
    for (int i = 0; i < cfg->stages; ++i) {
        capacity += cfg->stage[i].queue + cfg->stage[i].posts;
    }
    return capacity + 8 <= MAX_SLOTS ? 0 : -1;
}

int StNow(const State *s) {
    return ModelMinute(s->startNs, s->cfg.delayMs);
}

void StCarName(const State *s, int slot, char *buf, int size) {
    snprintf(buf, size, "car #%d", s->car[slot].id);
}

void StPostName(const State *s, int stage, int post, char *buf, int size) {
    snprintf(buf, size, "%s.%d", s->cfg.stage[stage].name, post + 1);
}

/// Событие от имени машины.
#define CAR_EVENT(s, slot, col, ...)                                                               \
    do {                                                                                           \
        char who_[24];                                                                             \
        StCarName((s), (slot), who_, sizeof(who_));                                                \
        OutEvent(StNow(s), who_, (s)->car[slot].pid, (col), __VA_ARGS__);                          \
    } while (0)

/// Событие от имени поста.
#define POST_EVENT(s, st, p, col, ...)                                                             \
    do {                                                                                           \
        char who_[24];                                                                             \
        StPostName((s), (st), (p), who_, sizeof(who_));                                            \
        OutEvent(StNow(s), who_, (s)->post[st][p].pid, (col), __VA_ARGS__);                        \
    } while (0)

void StCheck(State *s, int ok, const char *what) {
    if (!ok) {
        ++s->violations;
        OutEvent(StNow(s), "auditor", getpid(), C_FAIL, "INVARIANT VIOLATED: %s", what);
    }
}

int StReserveCar(State *s) {
    if (s->closed) {
        return -1;
    }
    for (int i = 0; i < MAX_SLOTS; ++i) {
        if (s->car[i].state == CAR_FREE) {
            CarSlot *c = &s->car[i];
            memset(c, 0, sizeof(*c));
            c->state = CAR_NEW;
            c->id = s->nextId++;
            c->waitStage = -1;
            c->post = -1;
            c->stage = -1;
            ++s->active;
            return i;
        }
    }
    return -1;
}

void StReleaseCar(State *s, int slot) {
    s->car[slot].state = CAR_FREE;
    s->car[slot].waitStage = -1;
    --s->active;
}

void StClose(State *s, const char *why) {
    if (s->closed) {
        return;
    }
    s->closed = 1;
    OutEvent(StNow(s), "entrance", getpid(), C_COORD, "entrance closed: %s", why);
}

int StChooseProgram(const Config *cfg, Rng *rng) {
    int total = 0;
    for (int i = 0; i < cfg->programs; ++i) {
        total += cfg->program[i].weight;
    }
    int r = RngRange(rng, 1, total);
    for (int i = 0; i < cfg->programs; ++i) {
        r -= cfg->program[i].weight;
        if (r <= 0) {
            return i;
        }
    }
    return cfg->programs - 1;
}

/// "prewash > tunnel > dry"
static void RouteText(const Config *cfg, int program, char *buf, int size) {
    const ProgramSpec *p = &cfg->program[program];
    int n = 0;
    buf[0] = '\0';
    for (int k = 0; k < p->len && n < size; ++k) {
        n += snprintf(buf + n, size - n, "%s%s", k ? " > " : "", cfg->stage[p->route[k]].name);
    }
}

/// Машина встаёт в накопитель стадии и в список ожидающих поста.
static void EnterQueue(State *s, int slot, int stage) {
    CarSlot *c = &s->car[slot];
    c->state = CAR_QUEUE;
    c->stage = stage;
    c->post = -1;
    c->waitStage = stage;
    c->seq = ++s->seq;
    ++s->queueLen[stage];
    StCheck(s, s->queueLen[stage] <= s->cfg.stage[stage].queue, "queue capacity exceeded (I5)");
}

int StArrive(State *s, int slot, int program, int pid, int gotPlace) {
    const Config *cfg = &s->cfg;
    CarSlot *c = &s->car[slot];
    c->pid = pid;
    c->program = program;
    c->step = 0;
    c->arrived = StNow(s);
    ++s->st.arrived;

    char route[80];
    RouteText(cfg, program, route, sizeof(route));
    CAR_EVENT(s, slot, C_CAR, "arrives, chooses %s (%s)", cfg->program[program].name, route);

    int dead = ConfigDeadStage(cfg, program);
    int first = cfg->program[program].route[0];
    if (dead >= 0) {
        ++s->st.refused;
        CAR_EVENT(s, slot, C_WAIT, "refused: stage %s has no posts, %s cannot be done",
                  cfg->stage[dead].name, cfg->program[program].name);
    } else if (!gotPlace) {
        ++s->st.turned;
        CAR_EVENT(s, slot, C_WAIT, "turned away: %s queue is full (%d/%d)", cfg->stage[first].name,
                  s->queueLen[first], cfg->stage[first].queue);
    } else {
        ++s->st.accepted;
        EnterQueue(s, slot, first);
        CAR_EVENT(s, slot, C_POST, "waits in %s queue (%d/%d)", cfg->stage[first].name,
                  s->queueLen[first], cfg->stage[first].queue);
        return 1;
    }
    StReleaseCar(s, slot);
    return 0;
}

void StBlocked(State *s, int slot) {
    const CarSlot *c = &s->car[slot];
    int next = s->cfg.program[c->program].route[c->step + 1];
    CAR_EVENT(s, slot, C_WAIT, "blocked on %s post %d: %s queue is full (%d/%d)",
              s->cfg.stage[c->stage].name, c->post + 1, s->cfg.stage[next].name, s->queueLen[next],
              s->cfg.stage[next].queue);
}

/// Машина уходит с поста: пост больше не связан с ней.
static int LeavePost(State *s, int slot) {
    CarSlot *c = &s->car[slot];
    int post = c->post;
    StCheck(s, c->state == CAR_POST && s->post[c->stage][post].car == slot,
            "car leaves a post it does not occupy (I2)");
    s->post[c->stage][post].car = -1;
    CAR_EVENT(s, slot, C_POST, "leaves %s post %d", s->cfg.stage[c->stage].name, post + 1);
    return post;
}

int StMoveOn(State *s, int slot) {
    CarSlot *c = &s->car[slot];
    const ProgramSpec *p = &s->cfg.program[c->program];
    StCheck(s, c->step + 1 < p->len, "car moves past the end of its route (I3)");
    int next = p->route[c->step + 1];
    ++c->step;
    if (s->cfg.stage[next].queue > 0) {
        int from = c->stage;
        int post = LeavePost(s, slot);
        EnterQueue(s, slot, next);
        CAR_EVENT(s, slot, C_POST, "moves from %s to %s queue (%d/%d)", s->cfg.stage[from].name,
                  s->cfg.stage[next].name, s->queueLen[next], s->cfg.stage[next].queue);
        return post;
    }
    // у стадии нет накопителя: ждём свободный пост, не уходя со своего
    c->waitStage = next;
    c->seq = ++s->seq;
    CAR_EVENT(s, slot, C_WAIT, "waits for a free %s post, staying on %s post %d",
              s->cfg.stage[next].name, s->cfg.stage[c->stage].name, c->post + 1);
    return -1;
}

int StFinish(State *s, int slot) {
    CarSlot *c = &s->car[slot];
    StCheck(s, c->step + 1 == s->cfg.program[c->program].len,
            "car leaves before finishing its route (I3)");
    int post = LeavePost(s, slot);
    int total = StNow(s) - c->arrived;
    int wait = total - c->opMinutes;
    ++s->st.finished;
    ++s->st.perProgram[c->program];
    s->st.totalMinutes += total;
    s->st.waitMinutes += wait > 0 ? wait : 0;
    if (total > s->st.maxMinutes) {
        s->st.maxMinutes = total;
    }
    CAR_EVENT(s, slot, C_DONE, "wash finished: %s in %d min (waited %d)",
              s->cfg.program[c->program].name, total, wait > 0 ? wait : 0);
    StReleaseCar(s, slot);
    return post;
}

int StHasWaiting(const State *s, int stage) {
    for (int i = 0; i < MAX_SLOTS; ++i) {
        if (s->car[i].state != CAR_FREE && s->car[i].waitStage == stage) {
            return 1;
        }
    }
    return 0;
}

/// Кто из двух ожидающих важнее по правилу: 1 - машина a.
static int Better(const State *s, const CarSlot *a, const CarSlot *b) {
    const Config *cfg = &s->cfg;
    if (cfg->rule == PRIO_PROGRAM) {
        int pa = cfg->program[a->program].priority, pb = cfg->program[b->program].priority;
        if (pa != pb) {
            return pa > pb;
        }
    } else if (cfg->rule == PRIO_SHORTEST) {
        int ra = cfg->program[a->program].len - a->step;
        int rb = cfg->program[b->program].len - b->step;
        if (ra != rb) {
            return ra < rb;
        }
    }
    return a->seq < b->seq;
}

int StDispatch(State *s, int stage, int post, int *queueFreed, int *prevStage, int *prevPost) {
    *queueFreed = 0;
    *prevStage = -1;
    *prevPost = -1;
    int best = -1;
    for (int i = 0; i < MAX_SLOTS; ++i) {
        const CarSlot *c = &s->car[i];
        if (c->state != CAR_FREE && c->waitStage == stage &&
            (best < 0 || Better(s, c, &s->car[best]))) {
            best = i;
        }
    }
    if (best < 0) {
        return -1;
    }
    CarSlot *c = &s->car[best];
    PostSlot *ps = &s->post[stage][post];
    StCheck(s, ps->state == POST_IDLE && ps->car < 0,
            "post takes a car while busy or broken (I1/I4)");
    StCheck(s, s->cfg.program[c->program].route[c->step] == stage,
            "stage is out of the program order (I3)");
    if (c->state == CAR_QUEUE) {
        StCheck(s, c->stage == stage, "car waits in a foreign queue (I2)");
        --s->queueLen[stage];
        *queueFreed = 1;
    } else {
        StCheck(s, c->state == CAR_POST, "car is neither in a queue nor on a post (I2)");
        *prevStage = c->stage;
        *prevPost = LeavePost(s, best);
    }
    c->state = CAR_POST;
    c->stage = stage;
    c->post = post;
    c->waitStage = -1;
    ps->state = POST_BUSY;
    ps->car = best;
    POST_EVENT(s, stage, post, C_POST, "car #%d enters %s post %d", c->id, s->cfg.stage[stage].name,
               post + 1);
    return best;
}

void StOpStart(State *s, int stage, int post, int minutes) {
    PostSlot *ps = &s->post[stage][post];
    StCheck(s, ps->state == POST_BUSY && ps->car >= 0, "operation without a car (I1)");
    POST_EVENT(s, stage, post, C_POST, "operation on car #%d starts (%d min)",
               ps->car >= 0 ? s->car[ps->car].id : 0, minutes);
}

void StOpEnd(State *s, int stage, int post, int minutes) {
    PostSlot *ps = &s->post[stage][post];
    ++ps->ops;
    ps->busyMinutes += minutes;
    if (ps->car >= 0) {
        s->car[ps->car].opMinutes += minutes;
        POST_EVENT(s, stage, post, C_POST, "operation on car #%d finished", s->car[ps->car].id);
    }
}

void StPostFree(State *s, int stage, int post) {
    PostSlot *ps = &s->post[stage][post];
    StCheck(s, ps->car < 0, "post is released while a car is still on it (I1)");
    if (ps->state == POST_BUSY) {
        ps->state = POST_IDLE;
    }
}

void StBreak(State *s, int stage, int post, int repair) {
    PostSlot *ps = &s->post[stage][post];
    ps->state = POST_BROKEN;
    ++ps->fails;
    POST_EVENT(s, stage, post, C_FAIL, "BREAKDOWN, repair takes %d min", repair);
}

void StRepair(State *s, int stage, int post) {
    s->post[stage][post].state = POST_IDLE;
    POST_EVENT(s, stage, post, C_FIX, "repaired, back to work");
}

void StStatus(const State *s) {
    const Config *cfg = &s->cfg;
    char clock[16];
    FormatClock(StNow(s), clock, sizeof(clock));
    OutLine("---- status at %s: %d car(s) on the line, entrance %s ----", clock, s->active,
            s->closed ? "closed" : "open");
    static const char *names[] = {"off", "idle", "busy", "BROKEN"};
    for (int st = 0; st < cfg->stages; ++st) {
        char line[200];
        int n = snprintf(line, sizeof(line), "  %-8s queue %d/%d |", cfg->stage[st].name,
                         s->queueLen[st], cfg->stage[st].queue);
        for (int p = 0; p < cfg->stage[st].posts && n < (int)sizeof(line); ++p) {
            const PostSlot *ps = &s->post[st][p];
            if (ps->car >= 0) {
                n += snprintf(line + n, sizeof(line) - n, " %d:car#%d", p + 1, s->car[ps->car].id);
            } else {
                n += snprintf(line + n, sizeof(line) - n, " %d:%s", p + 1, names[ps->state]);
            }
        }
        OutLine("%s", line);
    }
}

static const char *ReasonText(StopReason why) {
    switch (why) {
    case STOP_DONE: return "entrance closed and all accepted cars are washed";
    case STOP_IMPOSSIBLE: return "no program can be served";
    case STOP_INTERRUPT: return "interrupted by a signal";
    case STOP_FAILURE: return "a participant failed";
    default: return "?";
    }
}

int StExitCode(const State *s, StopReason why) {
    if (s->violations > 0) {
        return 4;
    }
    switch (why) {
    case STOP_DONE: return 0;
    case STOP_IMPOSSIBLE: return 2;
    case STOP_INTERRUPT: return 3;
    default: return 5;
    }
}

void StReport(const State *s, StopReason why) {
    const Config *cfg = &s->cfg;
    const Stats *st = &s->st;
    char from[16], to[16];
    FormatClock(0, from, sizeof(from));
    FormatClock(s->endMinute, to, sizeof(to));
    OutLine("================ Summary ================");
    OutLine("Model time: %s - %s (%d min)", from, to, s->endMinute);
    OutLine("Cars: arrived %d, accepted %d, washed %d, turned away %d (queue full), refused %d "
            "(impossible program), left unfinished %d",
            st->arrived, st->accepted, st->finished, st->turned, st->refused,
            st->accepted - st->finished);
    if (st->finished > 0) {
        OutLine("Time at the wash: avg %.1f min, max %d min; avg waiting %.1f min",
                (double)st->totalMinutes / st->finished, st->maxMinutes,
                (double)st->waitMinutes / st->finished);
    }
    char line[200];
    int n = snprintf(line, sizeof(line), "Programs washed:");
    for (int i = 0; i < cfg->programs && n < (int)sizeof(line); ++i) {
        n +=
            snprintf(line + n, sizeof(line) - n, " %s %d", cfg->program[i].name, st->perProgram[i]);
    }
    OutLine("%s", line);
    for (int i = 0; i < cfg->stages; ++i) {
        for (int p = 0; p < cfg->stage[i].posts; ++p) {
            const PostSlot *ps = &s->post[i][p];
            OutLine("  post %s.%d: %d operations, %d breakdowns, busy %d min (%d%%)",
                    cfg->stage[i].name, p + 1, ps->ops, ps->fails, ps->busyMinutes,
                    s->endMinute > 0 ? ps->busyMinutes * 100 / s->endMinute : 0);
        }
    }
    OutLine("Invariant violations: %d", s->violations);
    OutLine("Result: %s (exit code %d)", ReasonText(why), StExitCode(s, why));
}
