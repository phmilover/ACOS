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

#include "auditor.h"
#include "events.h"
#include "gantt.h"
#include "line.h"
#include "render.h"
#include "snapshot.h"
#include "soft_assert.h"
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

/// Живая схема: кадр со счётом аудитора - потоку отрисовки. Снимок берёт только крючок.
static void SubmitFrame(Snapshot *snap) {
    snap->checks = AuditorChecks();
    snap->violations = AuditorViolations();
    RenderSubmit(snap);
}

/// Конец модели: статус и последний кадр с ним (шапка «all cars washed» и т.п.).
static int Finish(Line *l, LineStatus status) {
    l->status = status;
    if (RenderActive()) {
        Snapshot snap;
        SnapshotTake(l, &snap);
        SubmitFrame(&snap);
    }
    return 0;
}

int QuiesceHook(int now, int nextWake, void *line) {
    Line *l = line;
    EventsSetClockSource();  // крючок зовут часы: STALL, INTERRUPT, CLOSE - от них
    // после SimKick крючок зовётся ещё раз в ту же минуту: колонка Ганта и кадр - один на минуту
    if (now != l->sampledAt) {
        Snapshot snap;
        SnapshotTake(l, &snap);
        GanttSample(&snap, l->cfg);
        SubmitFrame(&snap);
        l->sampledAt = now;
    }
    if (TakeRequest(l, REQ_SNAPSHOT)) {
        RenderFlush();  // не перемешать граф с кадром
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
        return Finish(l, LINE_INTERRUPTED);
    }
    if (!open && inside == 0) {
        return Finish(l, LINE_FINISHED);
    }
    if (atomic_load(&l->requests) & REQ_NO_RESOURCES) {
        return Finish(l, LINE_NO_RESOURCES);
    }
    if (Stalled(l, open, inside)) {
        EV(EV_STALL, -1, LineStallCause(l), -1, -1, inside);
        Finish(l, LINE_STALLED);
        RenderFlush();  // граф - под последним кадром
        WaitGraphPrint(1, l, isatty(1));
        if (l->cfg->dotFile[0] != '\0') {
            if (WaitGraphDot(l, l->cfg->dotFile) == 0) {
                dprintf(1, "Wait-for graph (DOT) saved to %s\n", l->cfg->dotFile);
            } else {
                perror("carwash-mt: cannot write the wait-for graph");
            }
        }
        return 0;
    }
    // продолжать, когда никто не спит, - ждать вечно: разбудить поток модели извне некому (поток
    // сигналов только будит часы). Так быть не должно: в модели всегда спят посты
    if (!SOFT_ASSERT(nextWake != -1, "quiescent with nobody to wake up")) {
        EV(EV_STALL, -1, -1, -1, -1, inside);
        return Finish(l, LINE_STALLED);
    }
    return 1;
}
