/**
 * @file quiesce.h
 * @brief Крючок часов в момент покоя: решает, продолжать ли модель.
 */
#ifndef QUIESCE_H
#define QUIESCE_H

/**
 * @brief SimHook: зовётся потоком часов, когда никто не работает.
 *
 * Въезд закрыт и внутри никого - LINE_FINISHED; запрос REQ_NO_RESOURCES - LINE_NO_RESOURCES.
 * В обоих случаях модель останавливается.
 * @param now текущая минута @param nextWake ближайшее пробуждение или -1
 * @param line Line *
 * @return 0 - остановить модель, 1 - продолжать
 */
int QuiesceHook(int now, int nextWake, void *line);

#endif
