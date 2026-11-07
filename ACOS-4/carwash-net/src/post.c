/*
 * Клиент-пост мойки. Получает от сервера «прими машину», сам выбирает длительность операции,
 * сообщает о начале и конце, после ухода машины может сломаться и чинится. Видит только свою
 * часть работы: какие машины к нему приезжали и что с ним самим происходило.
 *
 * Ctrl+C отключает только этот пост: сервер продолжает работу, а машину, над которой шла
 * операция, этот же пост домоет заново, когда его запустят снова.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "client.h"
#include "config.h"
#include "rng.h"
#include "util.h"

static Session ses;
static char who[24];

/// Что происходит с машиной на посту - в ленту этого клиента.
#define EVENT(col, ...) OutEvent(SessionNow(&ses), who, getpid(), (col), __VA_ARGS__)

typedef enum { GO_ON, GO_STOP, GO_GONE, GO_USER } Outcome;

static Outcome Classify(int r, const char *line) {
    if (r == -3) {
        return GO_USER;
    }
    if (r < 0) {
        return GO_GONE;
    }
    if (r == 1 && strncmp(line, "STOP ", 5) == 0) {
        return GO_STOP;
    }
    return GO_ON;
}

/// Ждёт @p minutes минут модели, следя за сервером. @p line - последняя строка.
static Outcome Work(int minutes, char *line, int size) {
    long long until = NowNs() + (long long)minutes * ses.delayMs * 1000000LL;
    for (;;) {
        int left = (int)((until - NowNs()) / 1000000LL);
        if (left <= 0) {
            return GO_ON;
        }
        Outcome o = Classify(SessionRecv(&ses, line, size, left), line);
        if (o != GO_ON) {
            return o;
        }
    }
}

static void Usage(int fd) {
    dprintf(fd, "Usage: cw-post SERVER PORT STAGE N\n"
                "  SERVER  server address or name, e.g. 127.0.0.1\n"
                "  PORT    server port\n"
                "  STAGE   stage name from the server config, e.g. tunnel\n"
                "  N       post number in the stage, from 1\n"
                "Ctrl+C takes this post off the line; start it again to bring it back.\n");
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "-h") == 0) {
        Usage(1);
        return 0;
    }
    int port, number;
    if (argc != 5 || ParseInt(argv[4], &number) != 0 || number < 1) {
        Usage(2);
        return 1;
    }
    if (NetParsePort(argv[2], &port) != 0) {
        return 1;
    }
    SignalsInstall();
    OutInit(-1, 0);
    char hello[80], line[LINE_MAX_LEN];
    snprintf(hello, sizeof(hello), "HELLO post %d %s %d", (int)getpid(), argv[3], number);
    if (SessionOpen(&ses, "cw-post", argv[1], port, 15, hello, line, sizeof(line)) != 0) {
        return 1;
    }
    char stage[NAME_LEN + 4];
    int num, tmin, tmax, rmin, rmax, now, open;
    unsigned seed;
    double failP;
    if (sscanf(line, "WELCOME post %15s %d %d %d %lf %d %d %u %d %d %d", stage, &num, &tmin,
               &tmax, &failP, &rmin, &rmax, &seed, &ses.delayMs, &now, &open) != 11) {
        dprintf(2, "cw-post: unexpected answer: %s\n", line);
        return 5;
    }
    SessionSync(&ses, now);
    snprintf(who, sizeof(who), "%s.%d", stage, num);
    Rng rng;
    RngInit(&rng, seed, 100U + (unsigned)(num * 31 + (int)stage[0]));
    dprintf(1, "cw-post: post %s is online at %s:%d (pid %d), operation %d-%d min, %s\n", who,
            argv[1], port, (int)getpid(), tmin, tmax,
            open ? "the line is working" : "waiting for the entrance to open");

    Outcome o = GO_ON;
    int car = 0, ops = 0, fails = 0;
    char program[NAME_LEN + 4] = "";
    while (o == GO_ON) {
        o = Classify(SessionRecv(&ses, line, sizeof(line), -1), line);
        if (o != GO_ON) {
            break;
        }
        if (sscanf(line, "OPEN %d", &now) == 1) {
            SessionSync(&ses, now);
            if (!open) {
                EVENT(C_COORD, "entrance opened, ready for cars");
            }
            open = 1;
        } else if (sscanf(line, "SERVE %d %15s", &car, program) == 2) {
            int minutes = RngRange(&rng, tmin, tmax);
            EVENT(C_CAR, "car #%d (%s) is on the post, operation takes %d min", car, program,
                  minutes);
            NetSend(ses.io.fd, "START %d", minutes);
            o = Work(minutes, line, sizeof(line));
            if (o != GO_ON) {
                break;
            }
            NetSend(ses.io.fd, "END %d", minutes);
            ++ops;
            EVENT(C_DONE, "operation on car #%d done, waiting for it to drive off", car);
        } else if (sscanf(line, "FREE %d", &car) == 1) {
            EVENT(C_POST, "car #%d drove off, the post is free", car);
            if (RngChance(&rng, failP)) {
                int repair = RngRange(&rng, rmin, rmax);
                ++fails;
                EVENT(C_FAIL, "BREAKDOWN, repair takes %d min", repair);
                NetSend(ses.io.fd, "BREAK %d", repair);
                o = Work(repair, line, sizeof(line));
                if (o != GO_ON) {
                    break;
                }
                NetSend(ses.io.fd, "REPAIR");
                EVENT(C_FIX, "repaired, back to work");
            }
            NetSend(ses.io.fd, "IDLE");
        }
    }
    int code = 0;
    if (o == GO_STOP) {
        code = SessionStopped(&ses, line);
    } else if (o == GO_USER) {
        dprintf(1, "cw-post: %s leaves the line (Ctrl+C); start it again to bring it back\n",
                who);
    } else {
        dprintf(2, "cw-post: the server is gone\n");
        code = 5;
    }
    dprintf(1, "cw-post: %s did %d operation(s), %d breakdown(s)\n", who, ops, fails);
    SessionBye(&ses);
    return code;
}
