/**
 * @file sync.h
 * @brief Мьютекс, условная переменная и rwlock с рангами и динамической проверкой порядка
 * (LOCKDEP).
 *
 * У каждого мьютекса и rwlock есть ранг. Поток обязан брать их в строго возрастающем порядке
 * рангов; захват с рангом не выше уже удерживаемого - нарушение. Так нельзя ни держать
 * два замка одного ранга, ни взять их в обратном порядке, а значит, цикл ожидания
 * между замками невозможен. Проверка идёт до блокировки, поэтому взаимная блокировка
 * превращается в сообщение, а не в зависание. Rwlock (на чтение или на запись) учитывается
 * как захват своего ранга. Ожидание на условной переменной стек LOCKDEP не меняет: мьютекс
 * остаётся в нём, пока поток спит, и возвращается захваченным.
 */
#ifndef SYNC_H
#define SYNC_H

#include <pthread.h>

#include "tsa.h"

/// Ранги замков: брать можно только в порядке возрастания.
enum {
    RANK_STAGE = 10,
    RANK_ENTRANCE = 20,
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

/// Создаёт мьютекс. @param m мьютекс @param rank ранг (RANK_*) @param name имя для сообщений, живёт
/// не меньше мьютекса
void MutexInit(Mutex *m, int rank, const char *name);

/// Уничтожает незахваченный мьютекс.
void MutexDestroy(Mutex *m);

/// Захватывает мьютекс; нарушение порядка рангов фиксируется до блокировки.
void MutexLock(Mutex *m) ACQUIRE(m);

/// Отпускает мьютекс, удерживаемый текущим потоком.
void MutexUnlock(Mutex *m) RELEASE(m);

/// Условная переменная pthread.
typedef struct {
    pthread_cond_t raw;
} Cond;

/// Создаёт условную переменную. @return 0 или -1
int CondInit(Cond *c);

/// Уничтожает условную переменную, на которой никто не ждёт.
void CondDestroy(Cond *c);

/// Ждёт сигнала, отпустив @p m; возвращается с захваченным @p m. Возможны ложные пробуждения:
/// вызывать в цикле по условию. Стек LOCKDEP не меняется.
void CondWait(Cond *c, Mutex *m) REQUIRES(m);

/// То же с ограничением по времени. @param ms предел ожидания, мс
/// @return 0 - разбудили (или ложное пробуждение), 1 - время истекло
int CondTimedWaitMs(Cond *c, Mutex *m, int ms) REQUIRES(m);

/// Будит одного ожидающего.
void CondSignal(Cond *c);

/// Будит всех ожидающих.
void CondBroadcast(Cond *c);

/// Rwlock pthread с рангом и именем для сообщений LOCKDEP.
typedef struct CAPABILITY("rwlock") RwLock {
    pthread_rwlock_t raw;
    int rank;
    const char *name;
} RwLock;

/// Создаёт rwlock. @param l замок @param rank ранг (RANK_*) @param name имя для сообщений
/// @return 0 или -1
int RwLockInit(RwLock *l, int rank, const char *name);

/// Уничтожает свободный rwlock.
void RwLockDestroy(RwLock *l);

/// Захватывает rwlock для чтения (читателей может быть несколько).
void RwLockRead(RwLock *l) ACQUIRE_SHARED(l);

/// Захватывает rwlock для записи (единолично).
void RwLockWrite(RwLock *l) ACQUIRE(l);

/// Отпускает rwlock, захваченный этим потоком (для чтения или записи).
void RwLockUnlock(RwLock *l) RELEASE_GENERIC(l);

/// Задаёт имя текущего потока для сообщений (строка должна жить дольше потока).
void LockdepSetThreadName(const char *name);

/// @return сколько замков (мьютексов и rwlock) сейчас держит текущий поток
int LockdepHeldCount(void);

/// @return сколько нарушений порядка найдено за прогон (во всех потоках)
long LockdepViolations(void);

/// Режим нарушения. @param fatal 1 (по умолчанию): сообщение в fd 2 и _exit(4); 0: сообщение и
/// продолжение
void LockdepSetFatal(int fatal);

/// Аварийный выход из любого потока: крючок LockdepSetExitHook (если задан), затем _exit(@p code).
/// Итоги и журнал при этом не печатаются. Так выходят фатальный LOCKDEP (4) и CheckedRealloc (5).
_Noreturn void FatalExit(int code);

/// Что сделать перед аварийным выходом FatalExit (например, вернуть курсор терминала).
/// NULL - ничего. Функция зовётся в потоке-нарушителе и не должна брать замки.
void LockdepSetExitHook(void (*hook)(void));

#endif
