/**
 * @file quiesce.h
 * @brief Крючок часов в момент покоя: решает, продолжать ли модель.
 */
#ifndef QUIESCE_H
#define QUIESCE_H

/**
 * @brief SimHook: зовётся потоком часов, когда никто не работает.
 *
 * По порядку: REQ_SNAPSHOT - граф ожидания и таблица потоков в stderr (бит снимается);
 * REQ_CLOSE - въезд закрывается (EV_CLOSE), бит снимается; REQ_INTERRUPT - EV_INTERRUPT,
 * LINE_INTERRUPTED; въезд закрыт и внутри никого - LINE_FINISHED; REQ_NO_RESOURCES -
 * LINE_NO_RESOURCES; тупик (внутри есть машины, LineProgressPossible ложно и въезд закрыт или
 * никого не пустит) - EV_STALL, граф ожидания в stdout (и в DOT при -g), LINE_STALLED. Во
 * всех случаях, кроме первых двух, модель останавливается.
 * @param now текущая минута @param nextWake ближайшее пробуждение (не используется)
 * @param line Line *
 * @return 0 - остановить модель, 1 - продолжать
 */
int QuiesceHook(int now, int nextWake, void *line);

#endif
