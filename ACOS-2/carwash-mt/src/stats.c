/**
 * @file stats.c
 * @brief Статистика (перенос из ИЗ1): считается только по событиям, а не по полям машин.
 *
 * Всё состояние ниже трогает StatsListener под мьютексом шины и StatsPrint после сбора
 * потоков, поэтому своих мьютексов нет.
 */

#include "stats.h"

#include <stdio.h>
#include <stdlib.h>

#include "memory.h"
#include "sim.h"

/// всё по программам; "ожидание" = время на мойке минус время самих операций
static int washed[MAX_PROGRAMS];
static long timeSum[MAX_PROGRAMS];
static long waitSum[MAX_PROGRAMS];
static int timeMax = 0;
static int blockedTimes = 0;
static long blockedMinutes = 0;   ///< сумма минут от EV_BLOCKED до следующего EV_MOVE машины
static int *blockedSince = NULL;  ///< минута EV_BLOCKED машины, -1 - не заблокирована
static int sinceCap = 0;

/// Учёт блокировки машины по номеру; массив растёт по мере приезда.
static int *BlockedSince(int car) {
    if (car >= sinceCap) {
        int newCap = sinceCap ? sinceCap : 64;
        while (newCap <= car) {
            newCap *= 2;
        }
        blockedSince = CheckedRealloc(blockedSince, (size_t)newCap * sizeof *blockedSince);
        for (int i = sinceCap; i < newCap; ++i) {
            blockedSince[i] = -1;
        }
        sinceCap = newCap;
    }
    return &blockedSince[car];
}

void StatsListener(const Event *e) {
    if (e->car < 0) {
        return;
    }
    switch (e->type) {
    case EV_BLOCKED:
        ++blockedTimes;
        *BlockedSince(e->car) = e->time;
        break;
    case EV_MOVE: {
        int *since = BlockedSince(e->car);
        if (*since >= 0) {
            blockedMinutes += e->time - *since;
            *since = -1;
        }
        break;
    }
    case EV_DONE:
        if (e->program >= 0 && e->program < MAX_PROGRAMS) {
            washed[e->program]++;
            timeSum[e->program] += e->arg;
            waitSum[e->program] += e->arg - e->arg2;
        }
        if (e->arg > timeMax) {
            timeMax = e->arg;
        }
        break;
    default: break;
    }
}

static const char *Outcome(LineStatus status) {
    switch (status) {
    case LINE_FINISHED: return "finished: entrance closed and every accepted car washed";
    case LINE_STALLED: return "STALLED: remaining cars can never be served";
    case LINE_INTERRUPTED: return "INTERRUPTED by user";
    case LINE_NO_RESOURCES: return "STOPPED: not enough resources (thread or semaphore)";
    default: return "running";
    }
}

/// Потоки, которые программа создаёт помимо машин: посты, координатор, часы, сигналы.
static int ServiceThreads(const Config *cfg) {
    int n = 3;
    for (int s = 0; s < cfg->stages; ++s) {
        n += cfg->stage[s].posts;
    }
    return n;
}

void StatsPrint(int fd, Line *l) {
    const Config *cfg = l->cfg;
    int now = SimNow();
    char from[16], to[16];
    EventClock(0, from, sizeof(from));
    EventClock(now, to, sizeof(to));
    MutexLock(&l->registry);
    int arrived = l->carCount, peak = l->peakAlive;
    MutexUnlock(&l->registry);
    MutexLock(&l->counters);
    int rejected = l->rejected, served = l->served, inside = l->inside, failures = l->failures;
    MutexUnlock(&l->counters);
    long allTime = 0, allWait = 0;
    for (int i = 0; i < cfg->programs; ++i) {
        allTime += timeSum[i];
        allWait += waitSum[i];
    }
    double hours = now / 60.0;

    dprintf(fd, "\n=================== RESULTS ===================\n");
    dprintf(fd, "Outcome        : %s\n", Outcome(l->status));
    dprintf(fd, "Model time     : %s - %s (%d min)\n", from, to, now);
    dprintf(fd, "Cars arrived   : %d (accepted %d, turned away %d)\n", arrived, arrived - rejected,
            rejected);
    dprintf(fd, "Cars washed    : %d, still inside: %d\n", served, inside);
    if (served > 0) {
        dprintf(fd, "Time at wash   : avg %.1f min, max %d min\n", (double)allTime / served,
                timeMax);
        dprintf(fd, "Waiting        : avg %.1f min per car (queues, blocked posts, repairs)\n",
                (double)allWait / served);
    }
    if (hours > 0) {
        dprintf(fd, "Throughput     : %.1f cars per hour\n", served / hours);
    }
    dprintf(fd, "Failures       : %d, blocked-post episodes: %d\n", failures, blockedTimes);
    dprintf(fd, "Blocked time   : %ld car-minutes waiting for room\n", blockedMinutes);
    for (int i = 0; i < cfg->programs; ++i) {
        dprintf(fd, "%s%-9s %3d washed", i == 0 ? "By program     : " : "                 ",
                cfg->program[i].name, washed[i]);
        if (washed[i] > 0) {
            dprintf(fd, ", avg %.1f min", (double)timeSum[i] / washed[i]);
        }
        dprintf(fd, "\n");
    }
    dprintf(fd, "Threads        : created %d (cars %d), peak alive cars %d, parks %ld\n",
            arrived + ServiceThreads(cfg), arrived, peak, SimTotalParks());
}

void StatsFree(void) {
    free(blockedSince);
    blockedSince = NULL;
    sinceCap = 0;
    for (int i = 0; i < MAX_PROGRAMS; ++i) {
        washed[i] = 0;
        timeSum[i] = waitSum[i] = 0;
    }
    timeMax = blockedTimes = 0;
    blockedMinutes = 0;
}
