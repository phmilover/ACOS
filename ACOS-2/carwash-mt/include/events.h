/**
 * @file events.h
 * @brief Потокобезопасная шина событий.
 *
 * Модель ничего не печатает, а только сообщает, что произошло. Лента, живая схема, аудитор,
 * Гант и статистика подписываются на события. Рассылка идёт под мьютексом шины (RANK_BUS):
 * у каждого события свой порядковый номер seq, подписчики видят события строго по порядку
 * номеров. Подписчик не читает живое состояние линии - событие несёт всё нужное; внутри
 * подписчика можно брать только мьютексы с рангом выше RANK_BUS (RANK_OUT) и нельзя звать
 * EventsEmit.
 */
#ifndef EVENTS_H
#define EVENTS_H

#include "config.h"
#include "sim.h"

typedef enum {
    EV_ARRIVE,
    EV_PROGRAM,  ///< arg = номер программы
    EV_REJECT,   ///< arg = сколько машин было в очереди
    EV_QUEUE,    ///< arg = сколько машин теперь в очереди
    EV_ENTER,
    EV_OP_START,  ///< arg = длительность
    EV_OP_END,
    EV_BLOCKED,  ///< arg = стадия, в которую не пускают
    EV_LEAVE,
    EV_MOVE,  ///< stage = куда, arg = откуда
    EV_FAIL,  ///< arg = время ремонта
    EV_REPAIR,
    EV_DONE,  ///< arg = минут на мойке, arg2 = минут операций
    EV_CLOSE,
    EV_STALL,  ///< stage = стадия без постов, из-за которой всё встало (или -1)
    EV_INTERRUPT,
} EventType;

/// Одно событие. Поля car, stage, post, program равны -1, если к событию не относятся.
typedef struct {
    long seq;  ///< сквозной номер, проставляет EventsEmit (первым: без дыры выравнивания)
    EventType type;
    int time;  ///< минута модели, проставляет EventsEmit
    int car, stage, post, program;
    int arg, arg2;  ///< смысл зависит от типа, см. EventType
    /// Кто отправил, проставляет EventsEmit по EventsSetSource. TH_CAR: srcId = номер машины;
    /// TH_POST: srcId = стадия * MAX_POSTS + пост (пост с нуля); TH_COORD: srcId 0 -
    /// координатор. srcId = -1 (с любым srcKind) - «система»: главный поток, часы, тесты,
    /// то есть любой поток, не звавший EventsSetSource.
    ThreadKind srcKind;
    int srcId;
} Event;

/// Подписчик получает событие под мьютексом шины.
typedef void (*Listener)(const Event *e);

/// Готовит шину: нет подписчиков, seq с нуля. Звать, пока других потоков нет.
/// @param cfg конфигурация для имён стадий и программ; живёт дольше шины
void EventsInit(const Config *cfg);

/// Добавляет подписчика (рассылка в порядке подписки). Звать до запуска потоков.
void EventsSubscribe(Listener l);

/// Запоминает для текущего потока источник следующих EventsEmit. NULL - вернуть «систему».
void EventsSetSource(const SimThread *self);

/// Проставляет seq, time = SimNow() и источник, рассылает подписчикам под мьютексом шины.
/// Звать надо под мьютексом той стадии, которая изменилась: тогда порядок событий совпадает с
/// порядком изменений.
void EventsEmit(Event e);

/// Короткая запись события: поля, не названные в макросе, нулевые (arg2 = 0).
#define EV(type_, car_, stage_, post_, program_, arg_)                                             \
    EventsEmit((Event){.type = (type_),                                                            \
                       .car = (car_),                                                              \
                       .stage = (stage_),                                                          \
                       .post = (post_),                                                            \
                       .program = (program_),                                                      \
                       .arg = (arg_)})

/// Текст события для ленты; живое состояние линии не читает. @return длина или 0
int EventDescribe(const Event *e, char *buf, int size);

/// Кто отправил: "car #7", "post tunnel.2" (пост с единицы), "coordinator", "main".
int EventSource(const Event *e, char *buf, int size);

/// ANSI-цвет строки события: поломки красные, ожидание жёлтое и т.д.
const char *EventColor(EventType type);

/// "08:17", а со вторых суток "d2 08:17".
int EventClock(int minute, char *buf, int size);

#endif
