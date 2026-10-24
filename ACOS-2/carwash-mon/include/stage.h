/**
 * @file stage.h
 * @brief Монитор стадии: въезд, ожидание места, бронь, переезд, освобождение места.
 *
 * Все функции с REQUIRES(st->m) зовутся под мьютексом стадии; ожидающие (StageAwaitSeat,
 * StageAwaitOperation, StageReserve) отпускают его только внутри SimWaitOn. В отличие от
 * «эстафеты» carwash-mt, освобождающий место никого не выбирает и ничего не меняет за других:
 * он будит всех, кто ждёт этого условия, а каждый сам в цикле while проверяет свой предикат
 * «моя ли очередь» (StageQueueTurn, StageRoomTurn) - поэтому правило приоритета соблюдается,
 * кто бы ни проснулся первым, а ложные пробуждения ничего не ломают.
 *
 * Будит только тот, кто держит мьютекс стадии: сначала SimGrant каждому ждущему этого условия
 * (часы считают его работающим), затем CondBroadcast. Будят только тогда, когда предикат
 * хоть одного ждущего стал истинным: после каждого изменения, которое может освободить место
 * или сменить «первого» (машина уехала с поста, пост починился, машина пересела из очереди на
 * пост, ожидающий получил бронь, машина въехала в очередь).
 *
 * Переезд машины между стадиями: StageReserve на следующей (бронь), затем StageMove: не вкладывая
 * мьютексы, StageVacate на текущей и StageCommit на следующей (StageVacate ищет место машины
 * на посту стадии, а не по её полям). Бронь учитывается во вместимости.
 */
#ifndef STAGE_H
#define STAGE_H

#include "line.h"

typedef enum { ADMIT_SEAT, ADMIT_QUEUE, ADMIT_FULL } Admit;

/**
 * @brief Въезд с улицы, без ожидания.
 *
 * Свободное место на исправном посту - если очередь пуста (и без брони в ней) и нет ожидающих:
 * машина сразу садится (EV_ENTER, EV_OP_START). Иначе место в очереди (queued + queueReserved <
 * вместимость, при SAB_OVERFLOW - вместимость + 1), если нет ожидающих: EV_QUEUE. Иначе
 * ADMIT_FULL, и ничего не меняется.
 */
Admit StageEnter(Line *l, Stage *st, Car *car) REQUIRES(st->m);

/**
 * @brief Предикат машины очереди: её ли очередь садиться на пост.
 *
 * Истина, если car стоит в очереди st, она лучшая по правилу приоритета (при равной оценке -
 * кто раньше встал) и есть свободное место на исправном посту (при SAB_OVERFLOW последний пост
 * многопостовой стадии не ищется).
 * @param post куда записать найденный пост
 * @param seat куда записать найденное место
 * @return 1 - очередь car, место в *post, *seat; 0 - нет
 */
int StageQueueTurn(Line *l, Stage *st, const Car *car, int *post, int *seat) REQUIRES(st->m);

/**
 * @brief Машина очереди ждёт своей очереди и садится на пост.
 *
 * while (!StageQueueTurn) SimWaitOn(seatFree, WAIT_QUEUE); затем EV_ENTER, EV_OP_START
 * (длительность RngRange(&post->rng, tmin, tmax)), service += длительность. Место в очереди
 * освободилось - будит тех, чей предикат стал истинным (seatFree, room).
 * @return SIM_OK - машина на посту; SIM_STOP - модель останавливается (машина в очереди)
 */
SimStatus StageAwaitSeat(Line *l, Stage *st, Car *car) REQUIRES(st->m);

/**
 * @brief Машина на посту ждёт конца операции.
 *
 * while (left != 0) SimWaitOn(post->done) с причиной WAIT_REPAIR, если пост сломан, иначе
 * WAIT_OPERATION. Будит пост: StageWakePost.
 * @return SIM_OK - операция кончилась; SIM_STOP - модель останавливается
 */
SimStatus StageAwaitOperation(Line *l, Stage *st, Car *car) REQUIRES(st->m);

/**
 * @brief Предикат ожидающего: можно ли car взять бронь на st.
 *
 * car должна быть первой среди ожидающих стадии (меньше ticket, затем выше оценка по правилу
 * приоритета, затем меньший номер машины); машина не из списка - только если список пуст.
 * Место: на посту (очередь пуста и без брони, есть свободное место исправного поста), иначе в
 * очереди.
 * @return RES_SEAT, RES_QUEUE - какую бронь можно взять; RES_NONE - нельзя
 */
Reservation StageRoomTurn(Line *l, Stage *st, const Car *car) REQUIRES(st->m);

/**
 * @brief Бронь места на st для машины, которая стоит на посту @p fromPost стадии @p from.
 *
 * Места нет с первого раза - машина встаёт в список ожидающих (ticket = ++nextTicket) и
 * сообщает EV_BLOCKED (stage = from, post = fromPost, arg = st); while (StageRoomTurn == RES_NONE)
 * SimWaitOn(room, WAIT_ROOM). Затем бронь (RES_SEAT / RES_QUEUE), выход из списка
 * (ticket = -1) и, если место ещё есть и ожидающие остались, - их пробуждение. Машина, у
 * которой бронь на st уже есть, сразу получает SIM_OK.
 * @return SIM_OK - бронь в car->res; SIM_STOP - модель останавливается (или список ожидающих
 * полон: SOFT_ASSERT), брони нет
 */
SimStatus StageReserve(Line *l, Stage *st, Car *car, int from, int fromPost) REQUIRES(st->m);

/**
 * @brief Бронь становится настоящим местом.
 *
 * На пост: EV_MOVE (если @p fromStage >= 0), EV_ENTER, EV_OP_START, service += длительность.
 * В очередь: EV_MOVE, EV_QUEUE (с SAB_OVERFLOW лишняя машина ловится SOFT_ASSERT "no room in
 * the queue"). car->step становится индексом этой стадии в маршруте. Затем будит тех, чей
 * предикат стал истинным.
 * @param fromStage откуда приехала машина, -1 - с въезда
 */
void StageCommit(Line *l, Stage *st, Car *car, int fromStage) REQUIRES(st->m);

/**
 * @brief Переезд машины с забронированным местом: StageVacate на @p from, затем StageCommit на
 * @p to, каждая под мьютексом своей стадии, не вкладывая их.
 *
 * Звать без захваченных мьютексов. Саботаж SAB_DEADLOCK: мьютекс @p to берётся, пока
 * мьютекс @p from ещё захвачен (вложенные мьютексы стадий одного ранга) - LOCKDEP ловит это
 * на первом же переезде любой машины, до того как могла бы случиться взаимная блокировка.
 */
void StageMove(Line *l, Stage *from, Stage *to, Car *car);

/// Машина освобождает своё место на посту стадии: EV_LEAVE, затем будит машины очереди
/// (seatFree) и ожидающих (room), если для них появилось место.
void StageVacate(Line *l, Stage *st, Car *car) REQUIRES(st->m);

/// Пост стадии починился (repair стал 0): машинам на исправных постах, ждущим конца операции,
/// возвращается причина WAIT_OPERATION; будит seatFree и room, если для них появилось место.
void StageRepaired(Line *l, Stage *st) REQUIRES(st->m);

/// Конец операции на посту: SimGrant каждой машине на местах поста (post->done), затем
/// CondBroadcast(&post->done). Зовёт поток поста.
void StageWakePost(Stage *st, Post *post) REQUIRES(st->m);

/// Оценка машины по правилу приоритета (больше - раньше): fifo 0, program - приоритет
/// программы, shortest - минус остаток работы.
int StageScore(const Line *l, const Car *car);

#endif
