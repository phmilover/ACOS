/**
 * @file quiesce.c
 * @brief Крючок момента покоя: снимок для Ганта, запросы сигналов, конец моделирования, тупик.
 *
 * Порядок разбора: снимок линии для диаграммы Ганта (раз в минуту), снимок (SIGUSR1), закрытие
 * въезда (SIGUSR2), прерывание (SIGINT/SIGTERM), конец, нехватка ресурсов, тупик. В покое никто из
 * потоков модели не работает, поэтому состояние линии устойчиво; счётчики линии - атомики,
 * мьютексы стадий запросы берут сами, по одному.
 */

#include "quiesce.h"

#include <stdio.h>
#include <unistd.h>

#include "events.h"
#include "gantt.h"
#include "line.h"
#include "snapshot.h"
#include "soft_assert.h"
#include "waitgraph.h"

/// Снимает бит запроса. @return 1, если он стоял
static int TakeRequest(Line *l, int bit) {
    return (atomic_fetch_and(&l->requests, (int)~(unsigned)bit) & bit) != 0;
}

/// SIGUSR2: закрыть въезд (если ещё открыт). Координатор увидит это и выйдет.
static void CloseEntrance(Line *l) {
    if (atomic_exchange(&l->open, 0)) {
        EV(EV_CLOSE, -1, -1, -1, -1, atomic_load(&l->inside));
    }
}

/// Тупик: внутри есть машины, ничто не может сдвинуться, а въезд закрыт или никого не пустит.
static int Stalled(Line *l, int open, int inside) {
    return inside > 0 && !LineProgressPossible(l) && !(open && LineEntranceMayAccept(l));
}

int QuiesceHook(int now, int nextWake, void *line) {
    Line *l = line;
    EventsSetClockSource();  // крючок зовут часы: STALL, INTERRUPT, CLOSE - от них
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
    int open = atomic_load(&l->open), inside = atomic_load(&l->inside);
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
        l->status = LINE_STALLED;
        WaitGraphPrint(1, l, isatty(1));
        if (l->cfg->dotFile[0] != '\0') {
            if (WaitGraphDot(l, l->cfg->dotFile) == 0) {
                dprintf(1, "Wait-for graph (DOT) saved to %s\n", l->cfg->dotFile);
            } else {
                perror("carwash-mon: cannot write the wait-for graph");
            }
        }
        return 0;
    }
    // продолжать, когда никто не спит, - ждать вечно: разбудить поток модели извне некому (поток
    // сигналов только будит часы). Так быть не должно: в модели всегда спят посты
    if (!SOFT_ASSERT(nextWake != -1, "quiescent with nobody to wake up")) {
        EV(EV_STALL, -1, -1, -1, -1, inside);
        l->status = LINE_STALLED;
        return 0;
    }
    return 1;
}
