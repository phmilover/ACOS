// Неименованные семафоры POSIX: sem_t лежат в разделяемом сегменте, pshared=1 разрешает
// пользоваться ими из разных процессов. Имён у них нет, удалять из системы нечего - достаточно
// sem_destroy и удаления сегмента. На macOS sem_init не реализован (ENOSYS).
#include <stddef.h>

#include "ipc_sems.h"

#ifdef __APPLE__
// sem_init объявлен устаревшим; вызов оставлен, чтобы получить ENOSYS на неподдерживаемой платформе
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

const char *IpcKind(void) {
    return "unnamed";
}

int SemsCreate(Ipc *ipc, const unsigned *values) {
    for (int i = 0; i < SEM_COUNT; ++i) {
        if (sem_init(&ipc->sh->sem[i], 1, values[i]) != 0) {
            return -1;
        }
        ipc->sem[i] = &ipc->sh->sem[i];
    }
    return 0;
}

int SemsAttach(Ipc *ipc) {
    for (int i = 0; i < SEM_COUNT; ++i) {
        ipc->sem[i] = &ipc->sh->sem[i];
    }
    return 0;
}

void SemsClose(Ipc *ipc) {
    for (int i = 0; i < SEM_COUNT; ++i) {
        ipc->sem[i] = NULL;
    }
}

void SemsDestroy(Ipc *ipc) {
    for (int i = 0; i < SEM_COUNT; ++i) {
        if (ipc->sem[i]) {
            sem_destroy(ipc->sem[i]);
            ipc->sem[i] = NULL;
        }
    }
}

void SemsRemoveStale(const char *key) {
    (void)key;  // неименованные семафоры исчезают вместе с сегментом
}
