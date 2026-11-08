/*
 * Программа 4, источник машин и сами машины. Источник ждёт открытия въезда на именованном
 * семафоре, затем через случайные интервалы просит у координатора ячейку и порождает процесс
 * машины. Машина регистрирует свой канал, выбирает программу и едет по маршруту: место в
 * следующем накопителе она берёт на именованном семафоре, о каждом шаге сообщает
 * координатору и ждёт от него «операция закончена».
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "proto.h"
#include "rng.h"
#include "state.h"
#include "util.h"

static const char *key = "cw4";
static Conn conn;
static sem_t *queueSem[MAX_STAGES];
static sem_t *openSem;
static int children = 0;

/// Ждёт сообщение; SIGINT пересылает координатору. @return как ConnRecv, но без -2
static int Receive(Kind kind, Msg *m, int timeoutMs) {
    for (;;) {
        // флаг - до ожидания: сигнал мог прийти вне select, и тогда EINTR не будет
        if (gotInterrupt) {
            gotInterrupt = 0;
            ConnSend(&conn, kind, M_INTERRUPT, 0, 0);
        }
        int r = ConnRecv(&conn, m, timeoutMs);
        if (r != -2) {
            return r;
        }
        if (timeoutMs >= 0) {
            return 0;
        }
    }
}

/// sem_wait, который пересылает SIGINT координатору и продолжает ждать.
static void WaitSem(sem_t *sem, Kind kind) {
    for (;;) {
        if (gotInterrupt) {
            gotInterrupt = 0;
            ConnSend(&conn, kind, M_INTERRUPT, 0, 0);
        }
        if (sem_wait(sem) == 0 || errno != EINTR) {
            return;
        }
    }
}

/// Тело процесса машины. @return код выхода
static int Car(int slot, int id) {
    // каналы источника достались по fork: закрываем копии, но FIFO источника не удаляем
    close(conn.mine);
    close(conn.coord);
    Config cfg;
    if (ConnOpen(&conn, key, K_CAR, slot, 0, NULL, &cfg, 5, "car") != 0) {
        return 5;
    }
    Rng rng;
    RngInit(&rng, cfg.seed, 1000U + (unsigned)id);
    int program = StChooseProgram(&cfg, &rng);
    const ProgramSpec *prog = &cfg.program[program];
    int first = prog->route[0];
    int gotPlace = ConfigDeadStage(&cfg, program) < 0 && cfg.stage[first].queue > 0 &&
                   sem_trywait(queueSem[first]) == 0;
    ConnSend(&conn, K_CAR, M_ARRIVE, program, gotPlace);
    if (!gotPlace) {
        ConnClose(&conn);
        return 0;
    }
    int code = 0;
    for (int step = 0;; ++step) {
        Msg m;
        int r = Receive(K_CAR, &m, -1);
        if (r < 0 || m.type == M_STOP) {
            code = r < 0 ? 5 : 3;
            break;
        }
        if (m.type != M_DONE) {
            continue;
        }
        if (step + 1 == prog->len) {
            ConnSend(&conn, K_CAR, M_LEAVE, 0, 0);
            ConnClose(&conn);
            return 0;
        }
        int next = prog->route[step + 1];
        if (cfg.stage[next].queue > 0 && sem_trywait(queueSem[next]) != 0) {
            // места нет: стоим на посту и ждём; координатор знает, что пост занят
            ConnSend(&conn, K_CAR, M_BLOCKED, 0, 0);
            WaitSem(queueSem[next], K_CAR);
        }
        ConnSend(&conn, K_CAR, M_MOVE, 0, 0);
    }
    ConnSend(&conn, K_CAR, M_BYE, 0, 0);
    ConnClose(&conn);
    return code;
}

static void ReapFinished(void) {
    while (children > 0 && waitpid(-1, NULL, WNOHANG) > 0) {
        --children;
    }
}

/// Ждёт @p minutes минут модели. @return 0 или -1 (STOP или координатор пропал)
static int Pause(const Config *cfg, int minutes) {
    long long until = NowNs() + (long long)minutes * cfg->delayMs * 1000000LL;
    for (;;) {
        int left = (int)((until - NowNs()) / 1000000LL);
        if (left <= 0) {
            return 0;
        }
        Msg m;
        int r = Receive(K_CARS, &m, left);
        if (r < 0 || (r == 1 && m.type == M_STOP)) {
            return -1;
        }
    }
}

int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "k:h")) != -1) {
        if (opt == 'k') {
            key = optarg;
        } else {
            dprintf(opt == 'h' ? 1 : 2, "Usage: cw4-cars [-k KEY]\n");
            return opt == 'h' ? 0 : 1;
        }
    }
    SignalsInstall();
    Config cfg;
    if (ConnOpen(&conn, key, K_CARS, 0, 0, NULL, &cfg, 10, "cw4-cars") != 0) {
        return 1;
    }
    if (ProtoOpenSems(key, &cfg, queueSem, &openSem) != 0) {
        dprintf(2, "cw4-cars: cannot open semaphores: %s\n", strerror(errno));
        ConnSend(&conn, K_CARS, M_BYE, 0, 0);
        ConnClose(&conn);
        return 5;
    }
    dprintf(2, "cw4-cars: connected (pid %d), waiting for the entrance to open\n", (int)getpid());
    WaitSem(openSem, K_CARS);

    Rng rng;
    RngInit(&rng, cfg.seed, 1);
    int spawned = 0, code = 0;
    for (;;) {
        if (Pause(&cfg, RngRange(&rng, cfg.arrMin, cfg.arrMax)) != 0) {
            code = 3;
            break;
        }
        ConnSend(&conn, K_CARS, M_NEWCAR, 0, 0);
        Msg m;
        int r;
        while ((r = Receive(K_CARS, &m, -1)) == 1 && m.type != M_SLOT && m.type != M_STOP) {
        }
        if (r < 0 || m.type == M_STOP) {
            code = 3;
            break;
        }
        if (m.a < 0) {
            break;  // въезд закрыт (конец дня или SIGUSR2)
        }
        ReapFinished();
        pid_t pid = fork();
        if (pid == 0) {
            _exit(Car(m.a, m.b));
        }
        if (pid > 0) {
            ++children;
        }
        if (cfg.cars > 0 && ++spawned >= cfg.cars) {
            ConnSend(&conn, K_CARS, M_CLOSE, spawned, 0);
            break;
        }
    }
    // отключаемся, только когда все машины-потомки завершились
    while (children > 0) {
        if (waitpid(-1, NULL, 0) > 0) {
            --children;
        } else if (errno != EINTR) {
            break;
        }
    }
    ConnSend(&conn, K_CARS, M_BYE, 0, 0);
    ConnClose(&conn);
    return code;
}
