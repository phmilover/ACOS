/**
 * @file line.h
 * @brief Структуры линии (стадии, посты, места, машины), создание и запросы в момент покоя.
 *
 * Состояние каждой стадии - очередь, посты с местами и список ожидающих въезда - защищено
 * мьютексом этой стадии (RANK_STAGE). Мьютексы стадий не вкладываются друг в друга.
 * Поля машины (место, бронь, номер в списке ожидающих) меняются под мьютексом той стадии,
 * к которой относятся. Счётчики линии - под counters (RANK_LINE), реестр машин - под
 * registry (RANK_REGISTRY).
 *
 * Поля постов лежат в массиве внутри Stage, и пометить их GUARDED_BY(m) нельзя: поэтому все
 * функции, работающие с постами, принимают Stage *st и помечены REQUIRES(st->m). Остальные
 * поля помечены MEMBER_GUARDED_BY (см. tsa.h: под Apple clang такая пометка пустая).
 */
#ifndef LINE_H
#define LINE_H

#include <pthread.h>

#include "config.h"
#include "mutex.h"
#include "rng.h"
#include "sim.h"
#include "tsa.h"

/// Сколько машин может ждать места на одной стадии: каждая из них стоит на месте другой стадии.
#define MAX_WAITERS (MAX_STAGES * MAX_POSTS * MAX_CAPACITY)

/// Запросы потока сигналов (биты Line.requests), их разбирают часы в момент покоя.
enum { REQ_INTERRUPT = 1, REQ_SNAPSHOT = 2, REQ_CLOSE = 4, REQ_NO_RESOURCES = 8 };

/// Бронь места на следующей стадии: получена, но машина туда ещё не переехала.
typedef enum { RES_NONE, RES_QUEUE, RES_SEAT } Reservation;

/// Где машина.
typedef enum { CAR_ARRIVING, CAR_QUEUE, CAR_POST, CAR_DONE, CAR_REJECTED } CarPlace;

/// Машина; у каждой свой поток.
typedef struct Car {
    int id, program;
    int step;  ///< индекс стадии маршрута, на которой машина сейчас
    SimThread th;
    Rng rng;
    pthread_t tid;
    int joined;  ///< координатор уже собрал поток (pthread_join)
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
} Seat;

/// Пост стадии. Поля - под мьютексом своей стадии.
typedef struct Post {
    int stage, index, capacity;
    Seat seat[MAX_CAPACITY];
    int reserved;  ///< забронированных мест (left < 0)
    /// минут до конца ремонта, 0 - исправен. Пост с забронированным местом (reserved > 0) не
    /// ломается: бронь - обещанное место, машина уже едет к нему, и операция на нём начнётся
    int repair;
    SimThread th;
    Rng rng;  ///< длительности операций и поломки; только под мьютексом стадии
    pthread_t tid;
} Post;

/// Стадия: посты, очередь (накопитель) перед ними и список ожидающих места.
typedef struct Stage {
    int index;
    Mutex m;
    Post post[MAX_POSTS];                       ///< поля постов - под m (см. описание файла)
    int queue[MAX_QUEUE] MEMBER_GUARDED_BY(m);  ///< номера машин в порядке постановки
    int queued MEMBER_GUARDED_BY(m);
    int queueReserved MEMBER_GUARDED_BY(m);          ///< забронированных мест в очереди
    Car *waiting[MAX_WAITERS] MEMBER_GUARDED_BY(m);  ///< в порядке постановки
    int waiters MEMBER_GUARDED_BY(m);
} Stage;

typedef enum {
    LINE_RUNNING,
    LINE_FINISHED,      ///< въезд закрыт, все уехали
    LINE_STALLED,       ///< тупик
    LINE_INTERRUPTED,   ///< SIGINT / SIGTERM
    LINE_NO_RESOURCES,  ///< не создался поток или семафор
} LineStatus;

typedef struct Line {
    const Config *cfg;
    Stage stage[MAX_STAGES];

    Mutex counters;                          ///< RANK_LINE
    int open MEMBER_GUARDED_BY(counters);    ///< въезд открыт
    int inside MEMBER_GUARDED_BY(counters);  ///< принятые и ещё не уехавшие
    int arrivedCount MEMBER_GUARDED_BY(counters);
    int rejected MEMBER_GUARDED_BY(counters);
    int served MEMBER_GUARDED_BY(counters);
    int failures MEMBER_GUARDED_BY(counters);
    long nextTicket MEMBER_GUARDED_BY(counters);  ///< последний выданный номер ожидающего

    Mutex registry;                          ///< RANK_REGISTRY
    Car **cars MEMBER_GUARDED_BY(registry);  ///< номер машины = индекс; машинами владеет линия
    int carCount MEMBER_GUARDED_BY(registry);
    int carCap MEMBER_GUARDED_BY(registry);
    int peakAlive MEMBER_GUARDED_BY(registry);
    int alive MEMBER_GUARDED_BY(registry);

    _Atomic int requests;  ///< REQ_* (ставит поток сигналов)
    LineStatus status;     ///< меняют только часы в момент покоя
    SimThread coordTh;
    pthread_t coordTid;
    Rng coordRng;
} Line;

/**
 * @brief Готовит линию: мьютексы, пустые стадии, семафоры постов и координатора.
 *
 * Звать после SimInit, пока других потоков нет. Посты получают имена "post tunnel.2" (пост с
 * единицы) и order = (MAX_STAGES - s) * MAX_POSTS + p, координатор - order 1000: в -D
 * в одну минуту посты будятся от последней стадии к первой, затем координатор.
 * @param cfg конфигурация; живёт дольше линии
 * @return 0 или -1, если не открылся семафор (код выхода 5); при -1 уже открытое закрыто
 */
int LineInit(Line *l, const Config *cfg);

/// Закрывает семафоры, уничтожает мьютексы, освобождает машины реестра. Все потоки собраны.
void LineFree(Line *l);

/// Машина по номеру (сам берёт registry; машины в памяти не двигаются). @return NULL - нет такой
Car *LineCar(Line *l, int id);

/// Только в момент покоя: может ли что-то сдвинуться - кто-то моется, или чинят пост, который
/// кому-то нужен (логика AnyProgressPossible ИЗ1). Берёт мьютексы стадий по одному.
int LineProgressPossible(Line *l);

/// Только в момент покоя: пустят ли на въезд хоть кого-нибудь (место в очереди первой стадии
/// какой-нибудь программы или пустой пост, даже сломанный; логика EntranceMayAccept ИЗ1).
int LineEntranceMayAccept(Line *l);

/// Только в момент покоя: стадия без постов, в которую упёрлись машины, или -1.
int LineStallCause(Line *l);

/// Следующая стадия маршрута машины или -1, если эта последняя.
int CarNextStage(const Car *car, const Config *cfg);

/// Сколько машине ещё мыться: сумма средних времён стадий с текущей до конца маршрута.
int CarRemainingWork(const Car *car, const Config *cfg);

#endif
