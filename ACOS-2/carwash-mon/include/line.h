/**
 * @file line.h
 * @brief Структуры линии (стадии, посты, места, машины), создание и запросы в момент покоя.
 *
 * Каждая стадия - монитор: её мьютекс (RANK_STAGE) защищает очередь, посты с местами и список
 * ожидающих въезда, а условные переменные seatFree (машины очереди ждут места на посту), room
 * (машины с других стадий ждут места) и done у каждого поста (машины на посту ждут конца
 * операции) живут под этим мьютексом. Мьютексы стадий не вкладываются друг в друга.
 * Поля машины (место, бронь, номер в списке ожидающих) меняются под мьютексом той стадии,
 * к которой относятся. Счётчики линии - атомики. Реестр машин - под rwlock registry
 * (RANK_REGISTRY): пишет один координатор, раз на прибытие машины, а читают все потоки машин и
 * постов под мьютексами разных стадий одновременно (машина при каждой проверке «моя ли очередь»
 * берёт через LineCar соседей по очереди), снимок и граф ожидания.
 *
 * Поля постов лежат в массиве внутри Stage, и пометить их GUARDED_BY(m) нельзя: поэтому все
 * функции, работающие с постами, принимают Stage *st и помечены REQUIRES(st->m). Остальные
 * поля помечены MEMBER_GUARDED_BY (см. tsa.h: под Apple clang такая пометка пустая).
 */
#ifndef LINE_H
#define LINE_H

#include <pthread.h>
#include <stdatomic.h>

#include "barrier.h"
#include "config.h"
#include "rng.h"
#include "sim.h"
#include "sync.h"
#include "tsa.h"

/// Сколько машин может ждать места на одной стадии: каждая из них стоит на месте другой стадии.
#define MAX_WAITERS (MAX_STAGES * MAX_POSTS * MAX_CAPACITY)

/// Запросы потока сигналов (биты Line.requests), их разбирают часы в момент покоя.
enum { REQ_INTERRUPT = 1, REQ_SNAPSHOT = 2, REQ_CLOSE = 4, REQ_NO_RESOURCES = 8 };

/// Бронь места на следующей стадии: получена, но машина туда ещё не переехала.
typedef enum { RES_NONE, RES_QUEUE, RES_SEAT } Reservation;

/// Где машина.
typedef enum { CAR_ARRIVING, CAR_QUEUE, CAR_POST, CAR_DONE, CAR_REJECTED } CarPlace;

struct Line;

/// Машина; у каждой свой поток.
typedef struct Car {
    struct Line *line;  ///< линия, на которой машина (задаётся при создании)
    int id, program;
    int step;  ///< индекс стадии маршрута, на которой машина сейчас
    SimThread th;
    Rng rng;
    pthread_t tid;
    int joined;       ///< поток уже собран (pthread_join) или не создавался
    atomic_int over;  ///< машина уехала или получила отказ: поток можно собирать
    // место, бронь и очередь ожидания - под мьютексом той стадии, к которой относятся
    CarPlace place;
    int stage, post, seat;  ///< где стоит (-1 - не относится)
    Reservation res;
    int resStage, resPost, resSeat;  ///< что забронировано (место на посту - при RES_SEAT)
    long ticket;  ///< номер в списке ожидающих (глобальный счётчик), -1 - не ждёт
    int arrived, finished, service;  ///< минуты: приезд, отъезд, сумма операций
} Car;

/// Место на посту. car == -1 - свободно; left < 0 - забронировано машиной car, она ещё не
/// заехала; left > 0 - идёт операция; left == 0 - машину помыли, она ещё стоит (занято).
typedef struct {
    int car;
    int left;      ///< минут до конца операции
    int duration;  ///< длительность операции
    /// минута начала операции: пост считает её минуты со следующей. В обычном режиме машина
    /// может сесть на пост в ту же минуту, когда его поток уже отработал или ещё нет, - без
    /// этой отметки операция в одном случае шла бы на минуту короче
    int start;
} Seat;

/// Пост стадии. Поля - под мьютексом своей стадии.
typedef struct Post {
    struct Line *line;  ///< линия, на которой пост (задаёт LineInit)
    int stage, index, capacity;
    Seat seat[MAX_CAPACITY];
    int reserved;  ///< забронированных мест (left < 0)
    /// минут до конца ремонта, 0 - исправен. Пост с забронированным местом (reserved > 0) не
    /// ломается: бронь - обещанное место, машина уже едет к нему, и операция на нём начнётся
    int repair;
    Cond done;  ///< машины на местах поста ждут конца операции (с мьютексом стадии)
    SimThread th;
    Rng rng;  ///< длительности операций и поломки; только под мьютексом стадии
    pthread_t tid;
} Post;

/// Стадия: посты, очередь (накопитель) перед ними и список ожидающих места.
typedef struct Stage {
    int index;
    Mutex m;
    Cond seatFree;                              ///< машины очереди ждут места на посту
    Cond room;                                  ///< машины с других стадий ждут места
    Post post[MAX_POSTS];                       ///< поля постов - под m (см. описание файла)
    int queue[MAX_QUEUE] MEMBER_GUARDED_BY(m);  ///< номера машин в порядке постановки
    int queued MEMBER_GUARDED_BY(m);
    int queueReserved MEMBER_GUARDED_BY(m);          ///< забронированных мест в очереди
    Car *waiting[MAX_WAITERS] MEMBER_GUARDED_BY(m);  ///< порядок не важен: решает ticket
    int waiters MEMBER_GUARDED_BY(m);
} Stage;

typedef enum {
    LINE_RUNNING,
    LINE_FINISHED,      ///< въезд закрыт, все уехали
    LINE_STALLED,       ///< тупик
    LINE_INTERRUPTED,   ///< SIGINT / SIGTERM
    LINE_NO_RESOURCES,  ///< не создался поток или условная переменная
} LineStatus;

typedef struct Line {
    const Config *cfg;
    Stage stage[MAX_STAGES];

    atomic_int open;          ///< въезд открыт
    atomic_int inside;        ///< принятые и ещё не уехавшие
    atomic_int arrivedCount;  ///< машин, для которых въезд уже решён (принята или отказ)
    atomic_int rejected;
    atomic_int served;
    atomic_int failures;
    atomic_int alive;        ///< созданных и ещё не собранных потоков машин
    atomic_int peakAlive;    ///< максимум alive; растёт через atomic_compare_exchange_weak
    atomic_long nextTicket;  ///< последний выданный номер ожидающего

    RwLock registry;                         ///< RANK_REGISTRY
    Car **cars MEMBER_GUARDED_BY(registry);  ///< номер машины = индекс; машинами владеет линия
    int carCount MEMBER_GUARDED_BY(registry);
    int carCap MEMBER_GUARDED_BY(registry);

    /// RANK_ENTRANCE: с ним координатор ждёт decided, пока все cfg->cars машин не решат вопрос
    /// с въездом (arrivedCount == cfg->cars); будит последняя машина дня
    Mutex entrance;
    Cond decided;
    Barrier start;       ///< одновременный старт постов, координатора, часов и main (готовит main)
    int barrierThreads;  ///< участников барьера start (задаёт main, печатают итоги)

    _Atomic int requests;  ///< REQ_* (ставит поток сигналов)
    LineStatus status;     ///< меняют только часы в момент покоя
    int sampledAt;         ///< минута последнего снимка для Ганта (-1 - не было); только часы
    /// Саботаж SAB_RACE: потоки постов увеличивают его каждую минуту без мьютекса - гонка
    /// данных, которую должен поймать ThreadSanitizer. Без саботажа не трогается
    long raceTicks;
    SimThread coordTh;
    pthread_t coordTid;
    Rng coordRng;
} Line;

/**
 * @brief Готовит линию: мьютексы, условные переменные, rwlock реестра, пустые стадии, потоки
 * постов и координатора с точки зрения часов.
 *
 * Звать после SimInit, пока других потоков нет. Посты получают имена "post tunnel.2" (пост с
 * единицы) и order = (MAX_STAGES - s) * MAX_POSTS + p, координатор - order 1000: в -D
 * в одну минуту посты будятся от последней стадии к первой, затем координатор. Барьер start
 * не трогает: его создаёт и уничтожает main.
 * @param cfg конфигурация; живёт дольше линии
 * @return 0 или -1, если не создалась условная переменная или rwlock (код выхода 5); при -1
 * уже созданное уничтожено
 */
int LineInit(Line *l, const Config *cfg);

/// Уничтожает условные переменные, rwlock и мьютексы, освобождает машины реестра. Все потоки
/// собраны.
void LineFree(Line *l);

/// Машина по номеру (сам берёт registry на чтение; машины в памяти не двигаются).
/// @return NULL - нет такой
Car *LineCar(Line *l, int id);

/// Только в момент покоя: может ли что-то сдвинуться - кто-то моется, или чинят пост, который
/// кому-то нужен (логика AnyProgressPossible ИЗ1). Берёт мьютексы стадий по одному.
int LineProgressPossible(Line *l);

/// Только в момент покоя: пустят ли на въезд хоть кого-нибудь (место в очереди первой стадии
/// какой-нибудь программы или пустой пост, даже сломанный; логика EntranceMayAccept ИЗ1).
int LineEntranceMayAccept(Line *l);

/// Только в момент покоя: стадия без постов, в которую упёрлись машины, или -1.
int LineStallCause(Line *l);

/**
 * @brief Код выхода программы по исходу модели и счётчикам проверок.
 *
 * Нарушение важнее исхода: хоть одно нарушение инварианта (аудитор), порядка блокировок
 * (LOCKDEP в нефатальном режиме) или сработавший SOFT_ASSERT - код 4. Иначе по исходу: 0 -
 * всё вымыто, 2 - тупик, 3 - прервали, 5 - не хватило ресурсов.
 * @param audit нарушений аудитора @param lockdep нарушений LOCKDEP @param soft срабатываний
 * SOFT_ASSERT
 */
int LineExitCode(LineStatus status, long audit, long lockdep, long soft);

/// Следующая стадия маршрута машины или -1, если эта последняя.
int CarNextStage(const Car *car, const Config *cfg);

/// Сколько машине ещё мыться: сумма средних времён стадий с текущей до конца маршрута.
int CarRemainingWork(const Car *car, const Config *cfg);

#endif
