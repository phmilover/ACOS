/**
 * @file gantt.h
 * @brief Диаграмма Ганта по постам и поиск узкого места.
 *
 * Раз в модельную минуту часы в момент покоя снимают линию (snapshot.h) и отдают снимок
 * GanttSample; в конце печатается лента: `#` моет, `=` ждёт (помытая машина стоит на посту),
 * `X` ремонт, `.` простой.
 *
 * Потоки: GanttSample зовёт только поток часов (из крючка), GanttPrint и GanttFree - главный
 * поток после сбора всех потоков модели, поэтому своих мьютексов у диаграммы нет.
 */
#ifndef GANTT_H
#define GANTT_H

#include "config.h"
#include "snapshot.h"

/// Запоминает состояние каждого поста в минуту снимка (одна колонка на вызов).
void GanttSample(const Snapshot *s, const Config *cfg);

/// Печатает ленту, загрузку постов и узкое место. @param color 1 - раскрасить ANSI-цветами
void GanttPrint(int fd, const Config *cfg, int color);
void GanttFree(void);

#endif
