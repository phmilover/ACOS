/*
 * Программа 3, источник машин. Ждёт, пока координатор откроет въезд (SEM_OPEN), и через
 * случайные интервалы порождает процесс на каждую машину. Машина - дочерний процесс
 * источника, но с координатором и постами она общается только через разделяемую память
 * и именованные семафоры.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "model.h"
#include "util.h"

static int children = 0;

static void ReapFinished(void) {
    while (children > 0 && waitpid(-1, NULL, WNOHANG) > 0) {
        --children;
    }
}

static int ForkCar(Ipc *ipc, int slot) {
    ReapFinished();
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        _exit(MCar(ipc, slot));
    }
    ++children;
    return 0;
}

int main(int argc, char **argv) {
    const char *key = "cw3";
    int opt;
    while ((opt = getopt(argc, argv, "k:h")) != -1) {
        if (opt == 'k') {
            key = optarg;
        } else {
            dprintf(opt == 'h' ? 1 : 2, "Usage: cw3-cars [-k KEY]\n");
            return opt == 'h' ? 0 : 1;
        }
    }
    SignalsInstall();
    Ipc ipc;
    if (MAttach(&ipc, key, 10, "cw3-cars") != 0) {
        return 5;
    }
    if (MOpenJournal(&ipc) != 0) {
        IpcDetach(&ipc);
        return 5;
    }
    MLock(&ipc);
    int other = ipc.sh->carsPid;
    int busy = other != 0 && kill(other, 0) == 0;
    if (!busy) {
        ipc.sh->carsPid = getpid();
    }
    MUnlock(&ipc);
    if (busy) {
        dprintf(2, "cw3-cars: the car source is already running (pid %d)\n", other);
        IpcDetach(&ipc);
        return 1;
    }
    dprintf(2, "cw3-cars: connected (pid %d), waiting for the entrance to open\n", (int)getpid());
    MSignal(&ipc, SEM_COORD);

    if (MWait(&ipc, SEM_OPEN) == 0) {
        MArrivals(&ipc, ForkCar);
    }
    while (children > 0) {
        MHandleSignals(&ipc);
        if (waitpid(-1, NULL, 0) > 0) {
            --children;
        } else if (errno != EINTR) {
            break;
        }
    }
    MLock(&ipc);
    ipc.sh->carsPid = 0;
    MUnlock(&ipc);
    MSignal(&ipc, SEM_COORD);
    int code = atomic_load(&ipc.sh->stop) == STOP_INTERRUPT ? 3 : 0;
    IpcDetach(&ipc);
    return code;
}
