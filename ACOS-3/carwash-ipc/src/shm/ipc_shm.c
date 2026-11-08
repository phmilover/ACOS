#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "ipc.h"
#include "ipc_sems.h"

static void ShmName(const char *key, char *buf, int size) {
    snprintf(buf, size, "/%s.shm", key);
}

/// Начальные значения семафоров по конфигурации.
static void InitialValues(const Config *cfg, unsigned *v) {
    memset(v, 0, sizeof(unsigned) * SEM_COUNT);
    v[SEM_MUTEX] = 1;
    for (int s = 0; s < cfg->stages; ++s) {
        v[SEM_QUEUE(s)] = (unsigned)cfg->stage[s].queue;
    }
}

/// Объекты с этим ключом остались от прогона, чей координатор уже не жив? Тогда удаляет их.
/// @return 0 - удалены, -1 - их хозяин жив
static int RemoveStale(const char *key, char *err, int errSize) {
    char name[40];
    ShmName(key, name, sizeof(name));
    int fd = shm_open(name, O_RDWR, 0600);
    if (fd >= 0) {
        int pid = 0;
        Shared *old = mmap(NULL, sizeof(Shared), PROT_READ, MAP_SHARED, fd, 0);
        if (old != MAP_FAILED) {
            pid = old->coordPid;
            munmap(old, sizeof(Shared));
        }
        close(fd);
        if (pid > 0 && kill(pid, 0) == 0) {
            snprintf(err, errSize, "key '%s' is in use by a running coordinator (pid %d)", key,
                     pid);
            return -1;
        }
    }
    shm_unlink(name);
    SemsRemoveStale(key);
    return 0;
}

int IpcCreate(Ipc *ipc, const char *key, const State *init, char *err, int errSize) {
    memset(ipc, 0, sizeof(*ipc));
    snprintf(ipc->key, sizeof(ipc->key), "%s", key);
    char name[40];
    ShmName(key, name, sizeof(name));

    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0 && errno == EEXIST) {
        if (RemoveStale(key, err, errSize) != 0) {
            return -1;
        }
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    }
    if (fd < 0) {
        snprintf(err, errSize, "shm_open %s: %s", name, strerror(errno));
        return -1;
    }
    if (ftruncate(fd, sizeof(Shared)) != 0) {
        snprintf(err, errSize, "ftruncate: %s", strerror(errno));
        close(fd);
        shm_unlink(name);
        return -1;
    }
    ipc->sh = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);  // отображение остаётся и без дескриптора
    if (ipc->sh == MAP_FAILED) {
        snprintf(err, errSize, "mmap: %s", strerror(errno));
        shm_unlink(name);
        return -1;
    }
    memset(ipc->sh, 0, sizeof(Shared));
    ipc->sh->st = *init;
    atomic_init(&ipc->sh->stop, STOP_NONE);
    ipc->sh->coordPid = getpid();
    ipc->owner = 1;

    unsigned values[SEM_COUNT];
    InitialValues(&init->cfg, values);
    if (SemsCreate(ipc, values) != 0) {
        snprintf(err, errSize, "cannot create %s semaphores: %s", IpcKind(), strerror(errno));
        IpcDestroy(ipc);
        return -1;
    }
    return 0;
}

int IpcAttach(Ipc *ipc, const char *key, char *err, int errSize) {
    memset(ipc, 0, sizeof(*ipc));
    snprintf(ipc->key, sizeof(ipc->key), "%s", key);
    char name[40];
    ShmName(key, name, sizeof(name));
    int fd = shm_open(name, O_RDWR, 0600);
    if (fd < 0) {
        snprintf(err, errSize, "shm_open %s: %s (is the coordinator running?)", name,
                 strerror(errno));
        return -1;
    }
    ipc->sh = mmap(NULL, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (ipc->sh == MAP_FAILED) {
        snprintf(err, errSize, "mmap: %s", strerror(errno));
        ipc->sh = NULL;
        return -1;
    }
    if (SemsAttach(ipc) != 0) {
        snprintf(err, errSize, "cannot open semaphores: %s", strerror(errno));
        IpcDetach(ipc);
        return -1;
    }
    return 0;
}

void IpcDetach(Ipc *ipc) {
    SemsClose(ipc);
    if (ipc->sh) {
        munmap(ipc->sh, sizeof(Shared));
        ipc->sh = NULL;
    }
}

void IpcDestroy(Ipc *ipc) {
    char name[40];
    ShmName(ipc->key, name, sizeof(name));
    if (ipc->sh) {
        SemsDestroy(ipc);
        munmap(ipc->sh, sizeof(Shared));
        ipc->sh = NULL;
    }
    shm_unlink(name);
}
