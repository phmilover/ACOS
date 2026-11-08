/**
 * @file ipc_sems.h
 * @brief Внутренний интерфейс между ipc_shm.c и реализацией семафоров (именованных или нет).
 */
#ifndef IPC_SEMS_H
#define IPC_SEMS_H

#include "ipc.h"

/// Создаёт все семафоры; начальные значения - в @p values. @return 0 или -1 (errno сохранён)
int SemsCreate(Ipc *ipc, const unsigned *values);
/// Открывает уже созданные семафоры. @return 0 или -1
int SemsAttach(Ipc *ipc);
/// Закрывает семафоры этого процесса.
void SemsClose(Ipc *ipc);
/// Удаляет семафоры из системы (создатель).
void SemsDestroy(Ipc *ipc);
/// Удаляет семафоры, оставшиеся от упавшего прогона с ключом @p key.
void SemsRemoveStale(const char *key);

#endif
