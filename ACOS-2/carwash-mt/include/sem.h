/**
 * @file sem.h
 * @brief Обёртка над именованными семафорами POSIX (на macOS безымянные sem_init недоступны).
 *
 * SemOpen создаёт семафор под уникальным именем и сразу делает sem_unlink: имя
 * освобождается, а сам семафор живёт, пока не закрыт, и не остаётся в системе после падения.
 *
 * Тестовый крючок: переменная окружения CARWASH_FAIL_SEM_AFTER=N заставляет N-й и все
 * последующие вызовы SemOpen (счёт с нуля) вернуть -1 с errno=ENOSPC. Переменная
 * читается при каждом вызове.
 */
#ifndef SEM_H
#define SEM_H

#include <semaphore.h>

/// Семафор; нулевая ручка значит «не открыт».
typedef struct {
    sem_t *h;
} Sem;

/// Открывает семафор с начальным значением @p value. @return 0 или -1 (errno сохранён)
int SemOpen(Sem *s, unsigned value);

/// Ждёт семафор, повторяя при EINTR.
void SemWait(Sem *s);

/// @return 0, если семафор взят, -1, если его значение было 0
int SemTryWait(Sem *s);

/// Увеличивает семафор.
void SemPost(Sem *s);

/// Закрывает семафор.
void SemClose(Sem *s);

/// @return сколько семафоров открыто сейчас (для тестов на утечки)
long SemOpenCount(void);

#endif
