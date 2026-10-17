/**
 * @file snapshot.h
 * @brief Снимок линии в момент покоя: для диаграммы Ганта (и кадров живой схемы).
 */
#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include "config.h"
#include "line.h"
#include "sim.h"

/// Сколько машин показывает панель потоков (остальные - строкой «и ещё N»).
#define PANEL_CARS 8

/// Чем занят поток модели, для счётчиков панели потоков.
typedef enum {
    PH_SLEEP,   ///< спит до своей минуты
    PH_WASH,    ///< ждёт конца операции (машина моется)
    PH_QUEUE,   ///< ждёт свободный пост в очереди
    PH_ROOM,    ///< ждёт места на следующей стадии (заблокирована)
    PH_REPAIR,  ///< ждёт ремонта поста
    PH_OTHER,   ///< остальное: готов, работает, ждёт пробуждения
    PH_COUNT
} PanelPhase;

/// Машина, которая ждёт место или пост (строка панели потоков).
typedef struct {
    int car;       ///< номер машины с нуля
    WaitKind why;  ///< WAIT_ROOM или WAIT_QUEUE
    int stage;     ///< стадия, место на которой (или пост в очереди которой) она ждёт
    long sysId;    ///< системный номер потока, 0 - неизвестен
} PanelCar;

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
    int failures;  ///< поломок постов
    int status;    ///< LineStatus на момент снимка
    /// Программа машины в той же позиции, что queue и seat (для цвета)
    int queueProg[MAX_STAGES][MAX_QUEUE];
    int seatProg[MAX_STAGES][MAX_POSTS][MAX_CAPACITY];
    long checks, violations;  ///< аудитор: проверено событий и найдено нарушений (ставит крючок)
    // панель потоков: по видам ThreadKind - живых потоков всего и по занятиям
    int threads[3];
    int phase[3][PH_COUNT];
    long parks;  ///< SimTotalParks()
    PanelCar
        panel[PANEL_CARS];  ///< ждущие машины по возрастанию номера, место-ожидающие в приоритете
    int panelCars;          ///< сколько заполнено
} Snapshot;

/**
 * @brief Снимает копию линии.
 *
 * Звать только в момент покоя (из крючка часов), не внутри SimForEach: мьютексы стадий
 * берутся по одному, затем counters и registry, последним - мьютекс часов (панель потоков,
 * SimForEach). В покое никто не работает, поэтому снимок согласован. Панель потоков:
 * живые потоки по видам и занятиям и до PANEL_CARS машин с WAIT_ROOM / WAIT_QUEUE (машины с
 * WAIT_ROOM важнее; если не помещаются, остаются они).
 */
void SnapshotTake(Line *l, Snapshot *s);

#endif
