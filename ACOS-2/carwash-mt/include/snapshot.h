/**
 * @file snapshot.h
 * @brief Снимок линии в момент покоя: для диаграммы Ганта (и кадров живой схемы).
 */
#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include "config.h"
#include "line.h"

/// Копия состояния линии на одну минуту. Поля стадий, которых нет в конфиге, нулевые.
typedef struct {
    int now;     ///< минута модели
    int open;    ///< въезд открыт
    int inside;  ///< принятые и ещё не уехавшие
    int queued[MAX_STAGES];
    int queue[MAX_STAGES][MAX_QUEUE];  ///< номера машин в очереди в порядке постановки
    Seat seat[MAX_STAGES][MAX_POSTS][MAX_CAPACITY];
    int repair[MAX_STAGES][MAX_POSTS];  ///< минут до конца ремонта, 0 - исправен
    int served, rejected, arrived;
} Snapshot;

/**
 * @brief Снимает копию линии.
 *
 * Звать только в момент покоя (из крючка часов), не внутри SimForEach: мьютексы стадий
 * берутся по одному, затем counters и registry. В покое никто не работает, поэтому снимок
 * согласован.
 */
void SnapshotTake(Line *l, Snapshot *s);

#endif
