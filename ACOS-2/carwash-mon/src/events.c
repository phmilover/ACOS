#include "events.h"

#include <stdio.h>

#include "soft_assert.h"
#include "sync.h"

#define MAX_LISTENERS 8
#define DAY_START     (8 * 60)  // мойка открывается в 08:00

static Mutex busMu;
static int busReady = 0;  // мьютекс создан (EventsInit зовут, пока других потоков нет)

static Listener listeners[MAX_LISTENERS] GUARDED_BY(busMu);
static int listenerCount GUARDED_BY(busMu) = 0;
static long nextSeq GUARDED_BY(busMu) = 0;
static const Config *config;  // пишется в EventsInit до создания потоков, дальше только читается

static _Thread_local ThreadKind srcKind = TH_COORD;
static _Thread_local int srcId = -1;  // -1 - «система», EVENT_SRC_CLOCK - часы

void EventsInit(const Config *cfg) {
    if (!busReady) {
        MutexInit(&busMu, RANK_BUS, "bus");
        busReady = 1;
    }
    config = cfg;
    MutexLock(&busMu);
    listenerCount = 0;
    nextSeq = 0;
    MutexUnlock(&busMu);
}

void EventsSubscribe(Listener l) {
    MutexLock(&busMu);
    if (SOFT_ASSERT(listenerCount < MAX_LISTENERS, "too many listeners")) {
        listeners[listenerCount++] = l;
    }
    MutexUnlock(&busMu);
}

void EventsSetSource(const SimThread *self) {
    srcKind = self ? self->kind : TH_COORD;
    srcId = self ? self->id : -1;
}

void EventsSetClockSource(void) {
    srcKind = TH_COORD;
    srcId = EVENT_SRC_CLOCK;
}

void EventsEmit(Event e) {
    e.time = SimNow();  // до мьютекса шины: мьютекс часов младше по рангу
    e.srcKind = srcKind;
    e.srcId = srcId;
    MutexLock(&busMu);
    e.seq = nextSeq++;
    for (int i = 0; i < listenerCount; ++i) {
        listeners[i](&e);
    }
    MutexUnlock(&busMu);
}

const char *EventColor(EventType type) {
    switch (type) {
    case EV_FAIL: return "\033[1;31m";
    case EV_REPAIR: return "\033[1;32m";
    case EV_DONE: return "\033[32m";
    case EV_REJECT:
    case EV_BLOCKED: return "\033[33m";
    case EV_ARRIVE:
    case EV_PROGRAM: return "\033[36m";
    case EV_CLOSE:
    case EV_STALL:
    case EV_INTERRUPT: return "\033[1;35m";
    default: return "\033[0m";
    }
}

int EventClock(int minute, char *buf, int size) {
    int t = DAY_START + minute;
    int day = (t / (24 * 60)) + 1;
    if (day == 1) {
        return snprintf(buf, size, "%02d:%02d", t / 60, t % 60);
    }
    return snprintf(buf, size, "d%d %02d:%02d", day, (t / 60) % 24, t % 60);
}

int EventSource(const Event *e, char *buf, int size) {
    if (e->srcId == EVENT_SRC_CLOCK) {
        return snprintf(buf, size, "clock");
    }
    if (e->srcId < 0) {
        return snprintf(buf, size, "main");
    }
    switch (e->srcKind) {
    case TH_CAR: return snprintf(buf, size, "car #%d", e->srcId + 1);
    case TH_POST: {
        int s = e->srcId / MAX_POSTS;
        int p = e->srcId % MAX_POSTS;
        const char *name = s < config->stages ? config->stage[s].name : "?";
        return snprintf(buf, size, "post %s.%d", name, p + 1);
    }
    case TH_COORD:
    default: return snprintf(buf, size, "coordinator");
    }
}

int EventDescribe(const Event *e, char *buf, int size) {
    const Config *cfg = config;
    const char *st = e->stage >= 0 ? cfg->stage[e->stage].name : "";
    const char *prog = e->program >= 0 ? cfg->program[e->program].name : "";
    int id = e->car + 1;  // людям привычнее нумерация с 1
    int post = e->post + 1;

    switch (e->type) {
    case EV_ARRIVE: return snprintf(buf, size, "car #%d arrives at the entrance", id);
    case EV_PROGRAM: {
        // "Premium (prewash > tunnel > wax > dry)"
        const ProgramSpec *p = &cfg->program[e->arg];
        int n = snprintf(buf, size, "car #%d chooses %s (", id, p->name);
        for (int k = 0; k < p->len && n < size; ++k) {
            n += snprintf(buf + n, size - n, "%s%s", k ? " > " : "", cfg->stage[p->route[k]].name);
        }
        if (n < size) {
            n += snprintf(buf + n, size - n, ")");
        }
        return n;
    }
    case EV_REJECT:
        return cfg->stage[e->stage].queue == 0
                   ? snprintf(buf, size,
                              "car #%d turned away: no free %s post and no waiting space", id, st)
                   : snprintf(buf, size, "car #%d turned away: %s queue is full (%d/%d)", id, st,
                              e->arg, cfg->stage[e->stage].queue);
    case EV_QUEUE:
        return snprintf(buf, size, "car #%d waits in %s queue (%d/%d)", id, st, e->arg,
                        cfg->stage[e->stage].queue);
    case EV_ENTER: return snprintf(buf, size, "car #%d enters %s post %d", id, st, post);
    case EV_OP_START:
        return snprintf(buf, size, "%s post %d starts on car #%d %s (%d min)", st, post, id, prog,
                        e->arg);
    case EV_OP_END: return snprintf(buf, size, "%s post %d finished car #%d", st, post, id);
    case EV_BLOCKED:
        return snprintf(buf, size, "car #%d waits on %s post %d: %s is full", id, st, post,
                        cfg->stage[e->arg].name);
    case EV_LEAVE: return snprintf(buf, size, "car #%d leaves %s post %d", id, st, post);
    case EV_MOVE:
        return snprintf(buf, size, "car #%d moves %s -> %s", id, cfg->stage[e->arg].name, st);
    case EV_FAIL:
        return e->car >= 0 ? snprintf(buf, size,
                                      "%s post %d BREAKS DOWN with car #%d on it (repair %d min)",
                                      st, post, id, e->arg)
                           : snprintf(buf, size, "%s post %d BREAKS DOWN (repair %d min)", st, post,
                                      e->arg);
    case EV_REPAIR:
        return snprintf(buf, size, "%s post %d is repaired and back in service", st, post);
    case EV_DONE:
        return snprintf(buf, size, "car #%d is clean! %s done in %d min, drives away", id, prog,
                        e->arg);
    case EV_CLOSE:
        return snprintf(buf, size, "ENTRANCE CLOSED, %d accepted car(s) still inside", e->arg);
    case EV_STALL:
        return e->stage >= 0
                   ? snprintf(buf, size,
                              "STALL: %d car(s) can never be served, stage %s has no posts", e->arg,
                              st)
                   : snprintf(buf, size, "STALL: %d car(s) can never be served", e->arg);
    case EV_INTERRUPT:
        return snprintf(buf, size, "INTERRUPTED by user, %d car(s) left inside", e->arg);
    default: return 0;
    }
}
