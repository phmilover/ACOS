/**
 * @file barrier.h
 * @brief Барьер: все участники ждут друг друга и проходят вместе.
 *
 * Там, где есть pthread_barrier_t (_POSIX_BARRIERS > 0, не macOS), используется он. Иначе -
 * своя реализация на мьютексе и условной переменной. Она считает поколения: ожидающий ждёт
 * смены поколения, а не равенства счётчика, поэтому барьер можно использовать повторно, а
 * ложные пробуждения ничего не ломают.
 */
#ifndef BARRIER_H
#define BARRIER_H

#include <pthread.h>
#include <unistd.h>

#include "sync.h"

#if defined(_POSIX_BARRIERS) && _POSIX_BARRIERS > 0 && !defined(__APPLE__)
#define BARRIER_NATIVE 1
#endif

/// Барьер на заданное число участников.
typedef struct Barrier {
#ifdef BARRIER_NATIVE
    pthread_barrier_t raw;
#else
    Mutex m;
    Cond cv;
    unsigned count;
    unsigned arrived MEMBER_GUARDED_BY(m);
    unsigned long generation MEMBER_GUARDED_BY(m);
#endif
} Barrier;

/// Создаёт барьер. @param b барьер @param count число участников, не меньше 1 @return 0 или -1
int BarrierInit(Barrier *b, unsigned count);

/// Ждёт, пока придут все участники. @return 1 - ровно одному («последний пришёл»), 0 - остальным
int BarrierWait(Barrier *b);

/// Уничтожает барьер, на котором никто не ждёт.
void BarrierDestroy(Barrier *b);

/// @return "pthread_barrier" или "mutex+cond": какая реализация собрана (для -h и итогов)
const char *BarrierKind(void);

#endif
