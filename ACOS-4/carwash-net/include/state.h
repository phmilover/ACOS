/**
 * @file state.h
 * @brief Состояние линии и переходы машин между местами.
 *
 * State - обычная структура без указателей. Ею владеет только сервер: посты и источник машин
 * присылают ему сообщения по TCP, а сервер меняет состояние функциями St* последовательно,
 * в одном потоке, поэтому блокировки не нужны. Функции St* проверяют инварианты и печатают
 * события от имени той сущности, которая действует; лента та же, что в ИЗ3.
 *
 * Места машины: очередь (накопитель) стадии или пост стадии. Машина, у которой следующая
 * стадия без накопителя, ждёт свободный пост, оставаясь на текущем посту.
 */
#ifndef STATE_H
#define STATE_H

#include "config.h"
#include "rng.h"

/// Сколько машин одновременно может быть на линии. Каждая принятая машина занимает место в
/// накопителе или на посту, поэтому их не больше суммы вместимостей (проверяется при старте).
#define MAX_SLOTS 128

typedef enum {
    CAR_FREE,   ///< ячейка свободна
    CAR_NEW,    ///< машина подъехала, ещё не выбрала программу
    CAR_QUEUE,  ///< стоит в накопителе стадии stage
    CAR_POST,   ///< стоит на посту post стадии stage
} CarState;

/// Машина на линии.
typedef struct {
    int state;  ///< CarState
    int id;     ///< номер машины за день, с 1
    int pid;    ///< процесс источника машин, который её прислал
    int program;
    int step;       ///< номер стадии маршрута, где машина сейчас или куда ждёт
    int stage;      ///< где стоит (очередь или пост этой стадии)
    int post;       ///< номер поста при CAR_POST
    int waitStage;  ///< стадия, где машина записана в ожидающие поста, или -1
    long seq;       ///< порядок записи в ожидающие (для fifo)
    int arrived;    ///< минута прибытия
    int opMinutes;  ///< сколько минут шли операции над машиной
} CarSlot;

typedef enum {
    POST_OFF,     ///< клиент поста не подключён
    POST_IDLE,    ///< ждёт машину
    POST_BUSY,    ///< на посту машина car
    POST_BROKEN,  ///< в ремонте, новую операцию не начинает
} PostState;

typedef struct {
    int state;  ///< PostState
    int car;    ///< ячейка машины на посту или -1
    int pid;
    int ops;    ///< сколько операций выполнено
    int fails;  ///< сколько раз ломался
    int busyMinutes;
} PostSlot;

/// Итоговая статистика.
typedef struct {
    int arrived;   ///< подъехало к въезду
    int accepted;  ///< встало в первую очередь
    int turned;    ///< уехало: первая очередь полна
    int refused;   ///< уехало: программу выполнить невозможно
    int finished;  ///< вымыто
    long totalMinutes, waitMinutes;
    int maxMinutes;
    int perProgram[MAX_PROGRAMS];
} Stats;

/// Почему закончилась имитация.
typedef enum {
    STOP_NONE,
    STOP_DONE,        ///< въезд закрыт, все принятые машины вымыты
    STOP_IMPOSSIBLE,  ///< ни одну программу выполнить нельзя
    STOP_INTERRUPT,   ///< SIGINT/SIGTERM серверу
    STOP_FAILURE,     ///< кончились ресурсы
} StopReason;

typedef struct {
    Config cfg;
    long long startNs;  ///< момент открытия въезда (минута 0 модели)
    CarSlot car[MAX_SLOTS];
    PostSlot post[MAX_STAGES][MAX_POSTS];
    int queueLen[MAX_STAGES];
    long seq;
    int nextId;      ///< номер следующей машины
    int active;      ///< машин на линии и у въезда (зарезервированных ячеек)
    int closed;      ///< въезд закрыт
    int endMinute;   ///< минута окончания (для итогов)
    int violations;  ///< сколько раз нарушен инвариант
    Stats st;
} State;

/// Обнуляет состояние и копирует конфигурацию. Проверяет, что MAX_SLOTS хватит.
/// @return 0 или -1 (линия слишком вместительна)
int StateInit(State *s, const Config *cfg);
/// Текущая минута модели.
int StNow(const State *s);
/// "car #3", "post tunnel.1" - имя сущности для ленты.
void StCarName(const State *s, int slot, char *buf, int size);
void StPostName(const State *s, int stage, int post, char *buf, int size);

/// Проверка инварианта: при нарушении считает его и печатает событие.
void StCheck(State *s, int ok, const char *what);

/// Резервирует ячейку под новую машину. @return номер ячейки или -1 (въезд закрыт или мест нет)
int StReserveCar(State *s);
/// Освобождает ячейку (машина уехала) и уменьшает active.
void StReleaseCar(State *s, int slot);
/// Закрывает въезд с причиной для ленты. Повторный вызов ничего не делает.
void StClose(State *s, const char *why);

/// Машина выбирает программу по весам. @return номер программы
int StChooseProgram(const Config *cfg, Rng *rng);

/**
 * @brief Машина у въезда: событие прибытия и проверка, можно ли её обслужить.
 * @param gotPlace 1 - в накопителе первой стадии есть место для машины
 * @return 1 - машина принята и стоит в очереди первой стадии, 0 - уехала (ячейка освобождена)
 */
int StArrive(State *s, int slot, int program, int pid, int gotPlace);

/// Машина без места в накопителе стадии маршрута step+1 ждёт на посту.
void StBlocked(State *s, int slot);
/**
 * @brief Машина переходит к следующей стадии маршрута.
 *
 * Если у стадии есть накопитель и машина взяла в нём место, она уходит с поста в очередь;
 * иначе остаётся на посту и записывается в ожидающие поста. В обоих случаях машина
 * попадает в список ожидающих стадии.
 * @return номер поста, который машина освободила, или -1 (она осталась на посту)
 */
int StMoveOn(State *s, int slot);
/// Последняя стадия пройдена: машина уезжает. @return освобождённый пост
int StFinish(State *s, int slot);

/// Есть ли у стадии машина, которая ждёт пост.
int StHasWaiting(const State *s, int stage);
/**
 * @brief Свободный пост забирает машину из ожидающих по правилу приоритета.
 * @param[out] queueFreed  1 - машина ушла из накопителя (освободилось место)
 * @param[out] prevStage, prevPost  пост, с которого машина ушла (ожидала на посту), или -1
 * @return ячейка машины или -1 (ожидающих нет)
 */
int StDispatch(State *s, int stage, int post, int *queueFreed, int *prevStage, int *prevPost);
/// Пост начинает операцию над своей машиной длительностью @p minutes.
void StOpStart(State *s, int stage, int post, int minutes);
/// Пост закончил операцию над своей машиной.
void StOpEnd(State *s, int stage, int post, int minutes);
/// Машина освободила пост: пост снова свободен (если не сломан).
void StPostFree(State *s, int stage, int post);
void StBreak(State *s, int stage, int post, int repair);
void StRepair(State *s, int stage, int post);

/// Состояние линии: очереди, посты, машины (по SIGUSR1).
void StStatus(const State *s);
/// Итоги прогона: на экран и в журнал.
void StReport(const State *s, StopReason why);
/// Код выхода по причине остановки и нарушениям инвариантов.
int StExitCode(const State *s, StopReason why);

#endif
