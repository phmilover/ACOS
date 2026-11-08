// Именованные семафоры POSIX: каждый - отдельный объект ядра с именем "/<key>.<номер>".
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>

#include "ipc_sems.h"

const char *IpcKind(void) {
    return "named";
}

static void SemName(const char *key, int i, char *buf, int size) {
    snprintf(buf, size, "/%s.%d", key, i);
}

int SemsCreate(Ipc *ipc, const unsigned *values) {
    for (int i = 0; i < SEM_COUNT; ++i) {
        char name[40];
        SemName(ipc->key, i, name, sizeof(name));
        ipc->sem[i] = sem_open(name, O_CREAT | O_EXCL, 0600, values[i]);
        if (ipc->sem[i] == SEM_FAILED) {
            ipc->sem[i] = NULL;
            return -1;
        }
    }
    return 0;
}

int SemsAttach(Ipc *ipc) {
    for (int i = 0; i < SEM_COUNT; ++i) {
        char name[40];
        SemName(ipc->key, i, name, sizeof(name));
        ipc->sem[i] = sem_open(name, 0);
        if (ipc->sem[i] == SEM_FAILED) {
            ipc->sem[i] = NULL;
            return -1;
        }
    }
    return 0;
}

void SemsClose(Ipc *ipc) {
    for (int i = 0; i < SEM_COUNT; ++i) {
        if (ipc->sem[i]) {
            sem_close(ipc->sem[i]);
            ipc->sem[i] = NULL;
        }
    }
}

void SemsDestroy(Ipc *ipc) {
    SemsClose(ipc);
    SemsRemoveStale(ipc->key);
}

void SemsRemoveStale(const char *key) {
    for (int i = 0; i < SEM_COUNT; ++i) {
        char name[40];
        SemName(key, i, name, sizeof(name));
        sem_unlink(name);
    }
}
