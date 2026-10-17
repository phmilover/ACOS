/**
 * @file quiesce.c
 * @brief Крючок момента покоя: снимок для Ганта, запросы сигналов, конец моделирования, тупик.
 *
 * Порядок разбора: снимок линии для диаграммы Ганта (раз в минуту), снимок (SIGUSR1), закрытие
 * въезда (SIGUSR2), прерывание (SIGINT/SIGTERM), конец, нехватка ресурсов, тупик. В покое никто из
 * потоков модели не работает, поэтому состояние линии устойчиво; мьютексы всё равно берутся по
 * правилам рангов: counters не держим, пока зовём запросы стадий (порядок stage -> counters).
 */

#include "quiesce.h"

#include <stdio.h>
#include <unistd.h>

#include "events.h"
#include "gantt.h"
#include "line.h"
#include "snapshot.h"
#include "waitgraph.h"

/// Снимает бит запроса. @return 1, если он стоял
static int TakeRequest(Line *l, int bit) {
    return (atomic_fetch_and(&l->requests, (int)~(unsigned)bit) & bit) != 0;
}

/// SIGUSR2: закрыть въезд (если ещё открыт). Координатор увидит это и выйдет.
static void CloseEntrance(Line *l) {
    MutexLock(&l->counters);
    int wasOpen = l->open;
    l->open = 0;
    int inside = l->inside;
    MutexUnlock(&l->counters);
    if (wasOpen) {
        EV(EV_CLOSE, -1, -1, -1, -1, inside);
    }
}

/// Тупик: внутри есть машины, ничто не может сдвинуться, а въезд закрыт или никого не пустит.
static int Stalled(Line *l, int open, int inside) {
    return inside > 0 && !LineProgressPossible(l) && !(open && LineEntranceMayAccept(l));
}

int QuiesceHook(int now, int nextWake, void *line) {
    (void)nextWake;
    Line *l = line;
    // после SimKick крючок зовётся ещё раз в ту же минуту: колонка Ганта - одна на минуту
    if (now != l->sampledAt) {
        Snapshot snap;
        SnapshotTake(l, &snap);
        GanttSample(&snap, l->cfg);
        l->sampledAt = now;
    }
    if (TakeRequest(l, REQ_SNAPSHOT)) {
        int color = isatty(2);
        WaitGraphPrint(2, l, color);
        WaitGraphThreads(2, l->cfg);
    }
    if (TakeRequest(l, REQ_CLOSE)) {
        CloseEntrance(l);
    }
    MutexLock(&l->counters);
    int open = l->open, inside = l->inside;
    MutexUnlock(&l->counters);
    if (atomic_load(&l->requests) & REQ_INTERRUPT) {
        EV(EV_INTERRUPT, -1, -1, -1, -1, inside);
        l->status = LINE_INTERRUPTED;
        return 0;
    }
    if (!open && inside == 0) {
        l->status = LINE_FINISHED;
        return 0;
    }
    if (atomic_load(&l->requests) & REQ_NO_RESOURCES) {
        l->status = LINE_NO_RESOURCES;
        return 0;
    }
    if (Stalled(l, open, inside)) {
        EV(EV_STALL, -1, LineStallCause(l), -1, -1, inside);
        WaitGraphPrint(1, l, isatty(1));
        if (l->cfg->dotFile[0] != '\0') {
            if (WaitGraphDot(l, l->cfg->dotFile) == 0) {
                dprintf(1, "Wait-for graph (DOT) saved to %s\n", l->cfg->dotFile);
            } else {
                perror("carwash-mt: cannot write the wait-for graph");
            }
        }
        l->status = LINE_STALLED;
        return 0;
    }
    return 1;
}
