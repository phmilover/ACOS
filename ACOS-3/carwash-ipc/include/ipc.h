/**
 * @file ipc.h
 * @brief Разделяемая память POSIX и семафоры программ 1-3.
 *
 * Сегмент shm_open содержит State и служебные поля. Семафоры двух видов (выбираются при
 * сборке):
 *  - ipc_named.c   - именованные семафоры POSIX (sem_open), программы 1 и 3;
 *  - ipc_unnamed.c - неименованные семафоры (sem_init с pshared=1) прямо в сегменте,
 *                    программа 2.
 * Остальной код обращается к семафору по номеру и не знает, какого он вида.
 */
#ifndef IPC_H
#define IPC_H

#include <semaphore.h>
#include <stdatomic.h>

#include "state.h"

/// Номера семафоров.
enum {
    SEM_MUTEX,   ///< двоичный: защищает State (1)
    SEM_COORD,   ///< будит координатора: машина уехала, сигнал, регистрация (0)
    SEM_OPEN,    ///< программа 3: источник машин ждёт, пока зарегистрируются все посты (0)
    SEM_QUEUE0,  ///< свободные места в накопителе стадии s
    SEM_WORK0 = SEM_QUEUE0 + MAX_STAGES,            ///< машин, ждущих пост стадии s (0)
    SEM_POST0 = SEM_WORK0 + MAX_STAGES,             ///< пост s.p: машина ушла с поста (0)
    SEM_CAR0 = SEM_POST0 + MAX_STAGES * MAX_POSTS,  ///< машина: позвали на пост / операция всё
    SEM_COUNT = SEM_CAR0 + MAX_SLOTS,
};

#define SEM_QUEUE(s)   (SEM_QUEUE0 + (s))
#define SEM_WORK(s)    (SEM_WORK0 + (s))
#define SEM_POST(s, p) (SEM_POST0 + (s) * MAX_POSTS + (p))
#define SEM_CAR(c)     (SEM_CAR0 + (c))

/// Содержимое разделяемой памяти.
typedef struct {
    State st;                ///< меняется только под SEM_MUTEX
    atomic_int stop;         ///< StopReason; читается без мьютекса в циклах ожидания
    int coordPid;            ///< координатор (создатель сегмента)
    int carsPid;             ///< программа 3: источник машин, 0 - не запущен
    int posts;               ///< сколько процессов постов сейчас зарегистрировано
    char journal[PATH_LEN];  ///< абсолютный путь журнала для независимых процессов
    sem_t sem[SEM_COUNT];    ///< используются только неименованные семафоры
} Shared;

/// Доступ процесса к сегменту и семафорам.
typedef struct {
    Shared *sh;
    sem_t *sem[SEM_COUNT];  ///< указатели на семафоры (в сегменте или из sem_open)
    char key[24];           ///< префикс имён объектов: "/<key>.shm", "/<key>.<номер>"
    int owner;              ///< 1 - этот процесс создал объекты и удалит их
} Ipc;

/// "named" или "unnamed" - вид семафоров в этой сборке.
const char *IpcKind(void);

/**
 * @brief Создаёт сегмент и все семафоры с начальными значениями.
 *
 * Если объекты с таким ключом остались от упавшего прогона (их координатор уже не жив),
 * они удаляются и создаются заново.
 * @return 0 или -1 (причина в @p err)
 */
int IpcCreate(Ipc *ipc, const char *key, const State *init, char *err, int errSize);
/// Подключает независимый процесс к уже созданным объектам. @return 0 или -1
int IpcAttach(Ipc *ipc, const char *key, char *err, int errSize);
/// Отключает процесс (munmap, sem_close).
void IpcDetach(Ipc *ipc);
/// Создатель удаляет объекты из системы: sem_unlink/sem_destroy и shm_unlink.
void IpcDestroy(Ipc *ipc);

#endif
