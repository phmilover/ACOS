/**
 * @file view_live.h
 * @brief Живая схема линии (как в ИЗ1) и панель потоков.
 *
 * Кадр рисуется по снимку Snapshot, живое состояние линии не читается: снимок делает крючок
 * часов в момент покоя, рисует поток отрисовки (render.h). Последние события запоминает
 * подписчик шины под своим мьютексом (RANK_OUT) - шина держит RANK_BUS, и порядок 50 -> 60
 * верен. Лента событий в режиме live на экран не идёт, только в журнал (ViewLogInit(-1, ...)).
 */
#ifndef VIEW_LIVE_H
#define VIEW_LIVE_H

#include "config.h"
#include "events.h"
#include "snapshot.h"

/// Очищает экран и прячет курсор. @param fd куда рисовать @param cfg стадии, программы, день;
/// живёт дольше схемы
void ViewLiveInit(int fd, const Config *cfg);

/// Подписчик шины: запоминает последние события для нижней части кадра.
void ViewLiveListener(const Event *e);

/// Рисует кадр одним write. Звать из одного потока (поток отрисовки). В кадре показываются
/// события не позже минуты снимка.
void ViewLiveDraw(const Snapshot *s);

/// Возвращает курсор и сбрасывает цвета. Безопасно звать повторно и без ViewLiveInit; звать на
/// любом пути выхода после ViewLiveInit.
void ViewLiveEnd(void);

#endif
