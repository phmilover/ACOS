/**
 * @file stats.h
 * @brief Итоговая статистика: исход, сколько вымыто, время на мойке, ожидание места, потоки.
 *
 * Потоки: StatsListener зовёт шина под своим мьютексом, по одному событию; StatsPrint и
 * StatsFree - главный поток после сбора всех потоков модели. Своих мьютексов статистике не
 * нужно.
 */
#ifndef STATS_H
#define STATS_H

#include "events.h"
#include "line.h"

/// Подписчик шины: по EV_DONE - время на мойке и операций, по EV_BLOCKED/EV_MOVE - сколько
/// машины стояли на посту, дожидаясь места на следующей стадии. Ожидание считается при
/// переезде (EV_MOVE): машины, которые всё ещё ждали места, когда модель кончилась тупиком
/// или прерыванием, в Blocked time не попадают.
void StatsListener(const Event *e);

/**
 * @brief Печатает блок RESULTS (как в ИЗ1) и строки ИЗ2:
 *
 *     Blocked time   : 57 car-minutes waiting for room
 *     Threads        : created 75 (cars 60), peak alive cars 12, parks 913
 *
 * Звать в конце, после сбора (join) всех потоков. @param l линия (счётчики под мьютексами)
 */
void StatsPrint(int fd, Line *l);

/// Освобождает учёт машин и обнуляет статистику.
void StatsFree(void);

#endif
