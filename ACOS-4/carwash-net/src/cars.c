/*
 * Клиент - источник машин. Через случайные интервалы присылает серверу новую машину с
 * выбранной программой (по весам из конфигурации сервера) и видит только судьбу своих машин:
 * принята, развёрнута у полной очереди, получила отказ, вымыта.
 *
 * Ctrl+C отключает источник: машины, которые уже на линии, домоются, а новые не приезжают,
 * пока источник не запустят снова. Сервер помнит, сколько машин уже приехало, и сообщает
 * новому источнику, сколько ещё осталось.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "client.h"
#include "config.h"
#include "rng.h"
#include "state.h"
#include "util.h"

static Session ses;
static Config cfg;  ///< нужны только программы и их веса
static int isOpen = 0, closed = 0;
static int sent = 0, accepted = 0, turned = 0, refused = 0, washed = 0;

#define EVENT(who, col, ...) OutEvent(SessionNow(&ses), (who), getpid(), (col), __VA_ARGS__)

typedef enum { GO_ON, GO_STOP, GO_GONE, GO_USER } Outcome;

/// Разбирает строку сервера и печатает, что случилось с машиной.
static Outcome Handle(int r, const char *line) {
    if (r == -3) {
        return GO_USER;
    }
    if (r < 0) {
        return GO_GONE;
    }
    if (r == 0) {
        return GO_ON;
    }
    int id, minutes;
    char program[NAME_LEN + 4], status[16], who[24];
    if (strncmp(line, "STOP ", 5) == 0) {
        return GO_STOP;
    }
    if (sscanf(line, "OPEN %d", &minutes) == 1) {
        SessionSync(&ses, minutes);  // OPEN несёт текущую минуту: въезд мог открыться давно
        if (!isOpen) {
            EVENT("entrance", C_COORD, "%s",
                  minutes ? "the entrance is open, cars keep arriving"
                          : "entrance opened, cars start arriving");
        }
        isOpen = 1;
    } else if (strcmp(line, "CLOSED") == 0) {
        if (!closed) {
            EVENT("entrance", C_COORD,
                  "entrance closed, no more cars; waiting for the line to finish");
        }
        closed = 1;
    } else if (sscanf(line, "CAR %d %15s %15s", &id, program, status) == 3) {
        snprintf(who, sizeof(who), "car #%d", id);
        if (strcmp(status, "accepted") == 0) {
            ++accepted;
            EVENT(who, C_CAR, "arrives for %s: accepted, waits in the first queue", program);
        } else if (strcmp(status, "turned") == 0) {
            ++turned;
            EVENT(who, C_WAIT, "arrives for %s: the queue is full, drives away", program);
        } else {
            ++refused;
            EVENT(who, C_WAIT, "arrives for %s: refused, this program cannot be done", program);
        }
    } else if (sscanf(line, "WASHED %d %15s %d", &id, program, &minutes) == 3) {
        ++washed;
        snprintf(who, sizeof(who), "car #%d", id);
        EVENT(who, C_DONE, "%s done in %d min, drives home", program, minutes);
    }
    return GO_ON;
}

/**
 * @brief Ждёт @p minutes минут модели, обрабатывая сообщения сервера.
 * @param minutes -1 - пока не откроется въезд (если он закрыт - до остановки)
 */
static Outcome Pause(int minutes) {
    long long until = minutes >= 0 ? NowNs() + ((long long)minutes * ses.delayMs * 1000000LL) : 0;
    char line[LINE_MAX_LEN];
    for (;;) {
        int left = -1;
        if (minutes >= 0) {
            left = (int)((until - NowNs()) / 1000000LL);
            if (left <= 0) {
                return GO_ON;
            }
        }
        int r = SessionRecv(&ses, line, sizeof(line), left);
        Outcome o = Handle(r, line);
        if (o == GO_STOP) {
            SessionStopped(&ses, line);
        }
        if (o != GO_ON) {
            return o;
        }
        if (minutes >= 0 ? closed : isOpen && !closed) {
            return GO_ON;  // въезд закрылся - ждать интервал незачем; открылся - пора ехать
        }
    }
}

static void Usage(int fd) {
    dprintf(fd, "Usage: cw-cars SERVER PORT\n"
                "  SERVER  server address or name, e.g. 127.0.0.1\n"
                "  PORT    server port\n"
                "Ctrl+C stops new arrivals; start it again to continue the day.\n");
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "-h") == 0) {
        Usage(1);
        return 0;
    }
    int port;
    if (argc != 3) {
        Usage(2);
        return 1;
    }
    if (NetParsePort(argv[2], &port) != 0) {
        return 1;
    }
    SignalsInstall();
    OutInit(-1, 0);
    char hello[48], line[LINE_MAX_LEN];
    snprintf(hello, sizeof(hello), "HELLO cars %d", (int)getpid());
    if (SessionOpen(&ses, "cw-cars", argv[1], port, 15, hello, line, sizeof(line)) != 0) {
        return 1;
    }
    int arrMin, arrMax, remaining, now, nextId, off;
    unsigned seed;
    ConfigDefaults(&cfg);
    if (sscanf(line, "WELCOME cars %d %d %d %u %d %d %d %d %n", &arrMin, &arrMax, &remaining, &seed,
               &ses.delayMs, &now, &nextId, &cfg.programs, &off) != 8 ||
        cfg.programs < 1 || cfg.programs > MAX_PROGRAMS) {
        dprintf(2, "cw-cars: unexpected answer: %s\n", line);
        return 5;
    }
    const char *p = line + off;
    for (int i = 0; i < cfg.programs; ++i) {
        int used = 0;
        if (sscanf(p, "%11s %d %n", cfg.program[i].name, &cfg.program[i].weight, &used) != 2) {
            dprintf(2, "cw-cars: bad program list: %s\n", line);
            return 5;
        }
        p += used;
    }
    SessionSync(&ses, now);
    Rng rng;
    RngInit(&rng, seed, 1U + (unsigned)now);  // после переподключения - другая последовательность
    char left[40] = "no limit on cars";
    if (remaining >= 0) {
        snprintf(left, sizeof(left), "%d car(s) still to come", remaining);
    }
    dprintf(1, "cw-cars: car source is online at %s:%d (pid %d), a car every %d-%d min, %s\n",
            argv[1], port, (int)getpid(), arrMin, arrMax, left);

    Outcome o = Pause(-1);  // до OPEN
    while (o == GO_ON && !closed && remaining != 0) {
        o = Pause(RngRange(&rng, arrMin, arrMax));
        if (o != GO_ON || closed) {
            break;
        }
        // программу машина выбирает по своей последовательности (номер машины - как в ИЗ3)
        Rng carRng;
        RngInit(&carRng, seed, 1000U + (unsigned)(nextId + sent));
        NetSend(ses.io.fd, "ARRIVE %d", StChooseProgram(&cfg, &carRng));
        ++sent;
        if (remaining > 0) {
            --remaining;
            if (remaining == 0) {
                NetSend(ses.io.fd, "CLOSE %d", sent);
            }
        }
    }
    while (o == GO_ON) {
        o = Pause(-1);  // въезд закрыт или машины кончились: досматриваем свои до STOP
    }
    int code = 0;
    if (o == GO_USER) {
        dprintf(1, "cw-cars: the car source goes offline (Ctrl+C); start it again to continue\n");
    } else if (o == GO_GONE) {
        dprintf(2, "cw-cars: the server is gone\n");
        code = 5;
    }
    dprintf(1, "cw-cars: sent %d car(s): accepted %d, turned away %d, refused %d, washed %d\n",
            sent, accepted, turned, refused, washed);
    SessionBye(&ses);
    return code;
}
