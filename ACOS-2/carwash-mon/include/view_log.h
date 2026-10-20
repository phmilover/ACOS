/**
 * @file view_log.h
 * @brief Лента событий на экран и в журнал.
 */
#ifndef VIEW_LOG_H
#define VIEW_LOG_H

#include "events.h"

/**
 * @param screenFd     куда печатать ленту, -1 - не печатать (работает живая схема)
 * @param color        раскрашивать ли строки на экране
 * @param journalFd    файл журнала, -1 - без журнала
 * @param threadColumn добавлять ли колонку источника: "[08:17] car #7         | car #7 moves ..."
 *                     (источник выровнен по 14 символам)
 */
void ViewLogInit(int screenFd, int color, int journalFd, int threadColumn);

/// Подписчик шины. Каждая строка уходит одним write, строки разных потоков не перемешиваются.
void ViewLogListener(const Event *e);

#endif
