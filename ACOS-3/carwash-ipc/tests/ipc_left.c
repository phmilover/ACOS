/*
 * Остались ли в системе объекты IPC прогона с ключом KEY (для тестов и после kill -9).
 *   ipc-left KEY     пробует открыть сегмент "/KEY.shm", семафоры "/KEY.0", "/KEY.open",
 *                    "/KEY.q0" и каталог каналов /tmp/KEY; код 0 - ничего не осталось,
 *                    1 - что-то осталось (печатает что)
 *   ipc-left -r KEY  удаляет все объекты с этим ключом
 * На macOS именованные семафоры не видны в файловой системе, поэтому проверка идёт открытием.
 */
#include <dirent.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ipc.h"
#include "proto.h"

/// Удаляет сегмент, семафоры программ 1-4 и каталог каналов с ключом @p key.
static void Remove(const char *key) {
    char name[400];
    snprintf(name, sizeof(name), "/%s.shm", key);
    shm_unlink(name);
    for (int i = 0; i < SEM_COUNT; ++i) {
        snprintf(name, sizeof(name), "/%s.%d", key, i);
        sem_unlink(name);
    }
    for (int s = -1; s < MAX_STAGES; ++s) {
        ProtoSemName(key, s, name, sizeof(name));
        sem_unlink(name);
    }
    char dir[64];
    ProtoDir(key, dir, sizeof(dir));
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] != '.') {
                snprintf(name, sizeof(name), "%s/%s", dir, e->d_name);
                unlink(name);
            }
        }
        closedir(d);
        rmdir(dir);
    }
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "-r") == 0) {
        Remove(argv[2]);
        return 0;
    }
    if (argc != 2) {
        dprintf(2, "usage: ipc-left [-r] KEY\n");
        return 2;
    }
    const char *key = argv[1];
    char name[64];
    int left = 0;

    snprintf(name, sizeof(name), "/%s.shm", key);
    int fd = shm_open(name, O_RDONLY, 0);
    if (fd >= 0) {
        close(fd);
        dprintf(1, "left: shared memory %s\n", name);
        left = 1;
    }
    const char *sems[] = {"0", "1", "open", "q0"};
    for (unsigned i = 0; i < sizeof(sems) / sizeof(sems[0]); ++i) {
        snprintf(name, sizeof(name), "/%s.%s", key, sems[i]);
        sem_t *s = sem_open(name, 0);
        if (s != SEM_FAILED) {
            sem_close(s);
            dprintf(1, "left: semaphore %s\n", name);
            left = 1;
        }
    }
    struct stat sb;
    snprintf(name, sizeof(name), "/tmp/%s", key);
    if (stat(name, &sb) == 0) {
        dprintf(1, "left: pipe directory %s\n", name);
        left = 1;
    }
    return left;
}
