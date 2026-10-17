/**
 * @file mutex.h
 * @brief Мьютекс с рангом и динамической проверкой порядка захвата (LOCKDEP).
 *
 * У каждого мьютекса есть ранг. Поток обязан брать мьютексы в строго возрастающем порядке
 * рангов; захват с рангом не выше уже удерживаемого - нарушение. Так нельзя ни держать
 * два мьютекса одного ранга, ни взять их в обратном порядке, а значит, цикл ожидания
 * между мьютексами невозможен. Проверка идёт до блокировки, поэтому взаимная блокировка
 * превращается в сообщение, а не в зависание.
 */
#ifndef MUTEX_H
#define MUTEX_H

#include <pthread.h>

#include "tsa.h"

/// Ранги мьютексов: брать можно только в порядке возрастания.
enum {
    RANK_STAGE = 10,
    RANK_LINE = 20,
    RANK_REGISTRY = 30,
    RANK_SIM = 40,
    RANK_RENDER = 45,
    RANK_BUS = 50,
    RANK_OUT = 60
};

/// Мьютекс pthread с рангом и именем для сообщений LOCKDEP.
typedef struct CAPABILITY("mutex") Mutex {
    pthread_mutex_t raw;
    int rank;
    const char *name;
} Mutex;

/// Создаёт мьютекс. @param rank ранг (RANK_*) @param name имя для сообщений, живёт не меньше
/// мьютекса
void MutexInit(Mutex *m, int rank, const char *name);

/// Уничтожает незахваченный мьютекс.
void MutexDestroy(Mutex *m);

/// Захватывает мьютекс; нарушение порядка рангов фиксируется до блокировки.
void MutexLock(Mutex *m) ACQUIRE(m);

/// Отпускает мьютекс, удерживаемый текущим потоком.
void MutexUnlock(Mutex *m) RELEASE(m);

/// Задаёт имя текущего потока для сообщений (строка должна жить дольше потока).
void LockdepSetThreadName(const char *name);

/// @return сколько мьютексов сейчас держит текущий поток
int LockdepHeldCount(void);

/// @return сколько нарушений порядка найдено за прогон (во всех потоках)
long LockdepViolations(void);

/// Режим нарушения. @param fatal 1 (по умолчанию): сообщение в fd 2 и _exit(4); 0: сообщение и
/// продолжение
void LockdepSetFatal(int fatal);

/// Что сделать перед аварийным _exit(4) при нарушении (например, вернуть курсор терминала).
/// NULL - ничего. Функция зовётся в потоке-нарушителе и не должна брать мьютексы.
void LockdepSetExitHook(void (*hook)(void));

#endif
