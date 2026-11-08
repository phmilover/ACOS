/*
 * Программа 4, пост мойки. Получает от координатора «прими машину», сам выбирает длительность
 * операции, сообщает о начале и конце, после ухода машины может сломаться. Пока идёт
 * операция или ремонт, пост ждёт на своём канале с таймаутом, чтобы сразу увидеть STOP.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "proto.h"
#include "rng.h"
#include "util.h"

static Conn conn;

/// Ждёт сообщение; SIGINT пересылает координатору. @return как ConnRecv, но без -2
static int Receive(Msg *m, int timeoutMs) {
    for (;;) {
        // флаг - до ожидания: сигнал мог прийти вне select, и тогда EINTR не будет
        if (gotInterrupt) {
            gotInterrupt = 0;
            ConnSend(&conn, K_POST, M_INTERRUPT, 0, 0);
        }
        int r = ConnRecv(&conn, m, timeoutMs);
        if (r != -2) {
            return r;
        }
        if (timeoutMs >= 0) {
            return 0;  // время ожидания пересчитает вызывающий
        }
    }
}

/// Ждёт @p minutes минут модели. @return 0 или -1 (пришёл STOP или пропал координатор)
static int Work(const Config *cfg, int minutes) {
    long long until = NowNs() + (long long)minutes * cfg->delayMs * 1000000LL;
    for (;;) {
        int left = (int)((until - NowNs()) / 1000000LL);
        if (left <= 0) {
            return 0;
        }
        Msg m;
        int r = Receive(&m, left);
        if (r < 0 || (r == 1 && m.type == M_STOP)) {
            return -1;
        }
    }
}

int main(int argc, char **argv) {
    const char *key = "cw4";
    int opt;
    while ((opt = getopt(argc, argv, "k:h")) != -1) {
        if (opt == 'k') {
            key = optarg;
        } else {
            dprintf(opt == 'h' ? 1 : 2, "Usage: cw4-post [-k KEY] STAGE NUMBER\n"
                                        "  STAGE  stage name from the config, e.g. tunnel\n"
                                        "  NUMBER post number in the stage, from 1\n");
            return opt == 'h' ? 0 : 1;
        }
    }
    int number;
    if (argc - optind != 2 || ParseInt(argv[optind + 1], &number) != 0 || number < 1) {
        dprintf(2, "Usage: cw4-post [-k KEY] STAGE NUMBER\n");
        return 1;
    }
    SignalsInstall();
    Config cfg;
    if (ConnOpen(&conn, key, K_POST, 0, number - 1, argv[optind], &cfg, 10, "cw4-post") != 0) {
        return 1;
    }
    int stage = 0;
    for (int s = 0; s < cfg.stages; ++s) {
        if (strcmp(cfg.stage[s].name, argv[optind]) == 0) {
            stage = s;
        }
    }
    const StageSpec *spec = &cfg.stage[stage];
    Rng rng;
    RngInit(&rng, cfg.seed, 100U + (unsigned)(stage * MAX_POSTS + number - 1));
    dprintf(2, "cw4-post: post %s.%d connected (pid %d)\n", spec->name, number, (int)getpid());

    int code = 0;
    ConnSend(&conn, K_POST, M_IDLE, 0, 0);
    for (;;) {
        Msg m;
        int r = Receive(&m, -1);
        if (r < 0) {
            dprintf(2, "cw4-post: the coordinator is gone\n");
            code = 5;
            break;
        }
        if (m.type == M_STOP) {
            break;
        }
        if (m.type == M_SERVE) {
            int minutes = RngRange(&rng, spec->tmin, spec->tmax);
            ConnSend(&conn, K_POST, M_OPSTART, minutes, 0);
            if (Work(&cfg, minutes) != 0) {
                break;
            }
            ConnSend(&conn, K_POST, M_OPEND, minutes, 0);
        } else if (m.type == M_FREE) {
            // машина съехала: прежде чем принять следующую, пост может сломаться
            if (RngChance(&rng, cfg.failP)) {
                int repair = RngRange(&rng, cfg.repairMin, cfg.repairMax);
                ConnSend(&conn, K_POST, M_BREAK, repair, 0);
                if (Work(&cfg, repair) != 0) {
                    break;
                }
                ConnSend(&conn, K_POST, M_REPAIR, 0, 0);
            }
            ConnSend(&conn, K_POST, M_IDLE, 0, 0);
        }
    }
    ConnSend(&conn, K_POST, M_BYE, 0, 0);
    ConnClose(&conn);
    return code;
}
