/*
 * Программы 1 и 2: один родительский процесс-координатор порождает дочерние процессы.
 *
 *   координатор (родитель) ──fork──► пост × (число постов всех стадий)
 *        │ источник машин: через случайные интервалы ──fork──► машина #1, #2, ...
 *        └ ждёт, пока въезд закрыт и линия пуста, будит всех, собирает детей, печатает итоги
 *
 * Данные - в разделяемой памяти POSIX (shm_open + mmap). Сборка с ipc_named.c даёт cw1
 * (именованные семафоры), с ipc_unnamed.c - cw2 (неименованные семафоры в сегменте).
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "model.h"
#include "util.h"

static int children = 0;  // сколько дочерних процессов ещё не собрано waitpid

/// В дочернем процессе: если родителя убьют kill -9, ребёнок получит SIGTERM и выйдет, а не
/// останется навсегда ждать на семафоре. На macOS такого механизма нет.
static void DieWithParent(void) {
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
}

static int OnExtra(int opt, const char *arg) {
    (void)arg;
    if (opt == 'h') {
        dprintf(1,
                "Usage: %s [-c FILE] [-s SEED] [-d MS] [-l FILE] [-u] [-q]\n"
                "  -c FILE  configuration file (default: built-in, see data/normal.cfg)\n"
                "  -s SEED  random seed\n"
                "  -d MS    milliseconds per model minute (default 100)\n"
                "  -l FILE  journal file (default carwash.log)\n"
                "  -u       unlimited: no end of day and no car limit, stop with Ctrl+C\n"
                "  -q       quiet: events only in the journal, summary on screen\n"
                "Signals: SIGINT/SIGTERM stop, SIGUSR1 prints the line state,\n"
                "         SIGUSR2 closes the entrance early.\n",
                "carwash");
        exit(0);
    }
    return -1;
}

/// Собирает завершившихся детей, не блокируясь.
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
        DieWithParent();
        _exit(MCar(ipc, slot));
    }
    ++children;
    return 0;
}

int main(int argc, char **argv) {
    Config cfg;
    if (ConfigFromArgs(&cfg, argc, argv, "h", OnExtra) != 0) {
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
    char key[24], err[200];
    snprintf(key, sizeof(key), "cw%s-%d", IpcKind()[0] == 'n' ? "1" : "2", (int)getpid());
    Ipc ipc;
    if (IpcCreate(&ipc, key, &init, err, sizeof(err)) != 0) {
        dprintf(2, "%s: %s\n", argv[0], err);
        return 5;
    }
    State *st = &ipc.sh->st;

    OutLine("Car wash, variant 43: parent process and child processes, %s POSIX semaphores, "
            "POSIX shared memory /%s.shm",
            IpcKind(), key);
    ConfigPrint(&cfg, 1);
    ConfigPrint(&cfg, journal);

    StopReason why = STOP_NONE;
    if (ConfigPossiblePrograms(&cfg) == 0) {
        OutEvent(0, "coordinator", getpid(), C_FAIL,
                 "no program can be served: every route "
                 "needs a stage without posts");
        why = STOP_IMPOSSIBLE;
    } else {
        st->startNs = NowNs();
        for (int s = 0; s < cfg.stages && why == STOP_NONE; ++s) {
            for (int p = 0; p < cfg.stage[s].posts; ++p) {
                pid_t pid = fork();
                if (pid < 0) {
                    MRequestStop(&ipc, STOP_FAILURE);
                    why = STOP_FAILURE;
                    break;
                }
                if (pid == 0) {
                    DieWithParent();
                    _exit(MPost(&ipc, s, p));
                }
                ++children;
            }
        }
        if (why == STOP_NONE) {
            OutEvent(0, "coordinator", getpid(), C_COORD,
                     "entrance opened, %d post processes "
                     "started",
                     ConfigTotalPosts(&cfg));
            MArrivals(&ipc, ForkCar);
            why = MAwaitEnd(&ipc);
        }
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
    while (children > 0) {
        if (waitpid(-1, NULL, 0) > 0) {
            --children;
        } else if (errno != EINTR) {
            break;
        }
    }

    StReport(st, why);
    int code = StExitCode(st, why);
    IpcDestroy(&ipc);
    close(journal);
    return code;
}
