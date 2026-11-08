/*
 * Программа 3, координатор. Независимые процессы: cw3-coord, cw3-post (по одному на пост) и
 * cw3-cars (источник машин) запускаются отдельно, в любом порядке, хоть из разных терминалов.
 * Встречаются они по ключу -k: координатор создаёт сегмент "/<key>.shm" и именованные
 * семафоры "/<key>.<номер>", остальные к ним подключаются.
 *
 * Координатор ждёт регистрации всех постов и источника машин, открывает въезд (SEM_OPEN),
 * ждёт конца дня и пустой линии, затем останавливает всех и удаляет объекты IPC.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "model.h"
#include "util.h"

#define LEAVE_TIMEOUT_MS 5000  // сколько ждать, пока участники отключатся после остановки

static const char *key = "cw3";

static int OnExtra(int opt, const char *arg) {
    if (opt == 'k') {
        key = arg;
        return strlen(arg) < sizeof(((Ipc *)0)->key) ? 0 : -1;
    }
    if (opt == 'h') {
        dprintf(1, "Usage: cw3-coord [-k KEY] [-c FILE] [-s SEED] [-d MS] [-l FILE] [-u] [-q]\n"
                   "Then start, in any order and from any terminal:\n"
                   "  cw3-post [-k KEY] STAGE NUMBER   one process per post\n"
                   "  cw3-cars [-k KEY]                the car source\n"
                   "or run scripts/run3.sh to start everything at once.\n"
                   "Signals to any participant: SIGINT/SIGTERM stop the whole line;\n"
                   "to the coordinator: SIGUSR1 prints the line state, SIGUSR2 closes the "
                   "entrance.\n");
        exit(0);
    }
    return -1;
}

/// Ждёт, пока зарегистрируются все посты и источник машин. @return 0 или -1 (прервали)
static int AwaitParticipants(Ipc *ipc, int posts) {
    int shown = -1;
    for (;;) {
        MLock(ipc);
        int have = ipc->sh->posts;
        int cars = ipc->sh->carsPid != 0;
        MUnlock(ipc);
        if (have == posts && cars) {
            return 0;
        }
        if (have * 2 + cars != shown) {
            dprintf(1, "waiting for participants: posts %d/%d, car source %s\n", have, posts,
                    cars ? "connected" : "not connected");
            shown = have * 2 + cars;
        }
        if (MWait(ipc, SEM_COORD) != 0) {
            return -1;
        }
    }
}

int main(int argc, char **argv) {
    Config cfg;
    if (ConfigFromArgs(&cfg, argc, argv, "hk:", OnExtra) != 0) {
        return 1;
    }
    int journal = open(cfg.journal, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (journal < 0) {
        dprintf(2, "%s: cannot open journal %s\n", argv[0], cfg.journal);
        return 1;
    }
    OutInit(journal, cfg.quiet);
    SignalsInstall();

    State init;
    if (StateInit(&init, &cfg) != 0) {
        dprintf(2, "%s: the line holds too many cars (max %d)\n", argv[0], MAX_SLOTS);
        return 1;
    }
    char err[200];
    Ipc ipc;
    if (IpcCreate(&ipc, key, &init, err, sizeof(err)) != 0) {
        dprintf(2, "%s: %s\n", argv[0], err);
        return 5;
    }
    // участники могут работать в другом каталоге - им нужен абсолютный путь журнала
    char cwd[PATH_MAX];
    int len = cfg.journal[0] == '/' || getcwd(cwd, sizeof(cwd)) == NULL
                  ? snprintf(ipc.sh->journal, sizeof(ipc.sh->journal), "%s", cfg.journal)
                  : snprintf(ipc.sh->journal, sizeof(ipc.sh->journal), "%s/%s", cwd, cfg.journal);
    if (len >= (int)sizeof(ipc.sh->journal)) {
        dprintf(2, "%s: journal path is too long\n", argv[0]);
        IpcDestroy(&ipc);
        return 1;
    }
    State *st = &ipc.sh->st;

    OutLine("Car wash, variant 43: independent processes, named POSIX semaphores, POSIX shared "
            "memory /%s.shm (coordinator pid %d)",
            key, (int)getpid());
    ConfigPrint(&cfg, 1);
    ConfigPrint(&cfg, journal);

    StopReason why = STOP_NONE;
    if (ConfigPossiblePrograms(&cfg) == 0) {
        OutEvent(0, "coordinator", getpid(), C_FAIL,
                 "no program can be served: every route "
                 "needs a stage without posts");
        why = STOP_IMPOSSIBLE;
    } else if (AwaitParticipants(&ipc, ConfigTotalPosts(&cfg)) != 0) {
        why = (StopReason)atomic_load(&ipc.sh->stop);
    } else {
        MLock(&ipc);
        st->startNs = NowNs();
        OutEvent(0, "coordinator", getpid(), C_COORD,
                 "all %d posts and the car source are "
                 "here, entrance opened",
                 ConfigTotalPosts(&cfg));
        MUnlock(&ipc);
        MSignal(&ipc, SEM_OPEN);
        why = MAwaitEnd(&ipc);
    }

    MLock(&ipc);
    st->endMinute = st->startNs ? StNow(st) : 0;
    if (why == STOP_INTERRUPT) {
        OutEvent(st->endMinute, "coordinator", getpid(), C_COORD,
                 "interrupted: stopping all processes");
    }
    MUnlock(&ipc);
    MRequestStop(&ipc, why);
    MWakeAll(&ipc);
    // участники сами снимают регистрацию; ждём их, но не вечно (могли убить kill -9)
    for (int waited = 0; waited < LEAVE_TIMEOUT_MS; waited += 20) {
        MLock(&ipc);
        int left = ipc.sh->posts + (ipc.sh->carsPid != 0);
        MUnlock(&ipc);
        if (left == 0) {
            break;
        }
        SleepMs(20);
    }

    StReport(st, why);
    int code = StExitCode(st, why);
    IpcDestroy(&ipc);
    close(journal);
    return code;
}
