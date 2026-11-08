#include "proto.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

#define POLL_MS 20

void ProtoDir(const char *key, char *buf, int size) {
    snprintf(buf, size, "/tmp/%s", key);
}

void ProtoCoordPath(const char *key, char *buf, int size) {
    snprintf(buf, size, "/tmp/%s/coord", key);
}

void ProtoSemName(const char *key, int stage, char *buf, int size) {
    if (stage < 0) {
        snprintf(buf, size, "/%s.open", key);
    } else {
        snprintf(buf, size, "/%s.q%d", key, stage);
    }
}

/// Ждёт, пока в канале появятся данные. @return 1, 0 (время вышло), -2 (сигнал)
static int WaitReadable(int fd, int timeoutMs) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int r = select(fd + 1, &set, NULL, NULL, timeoutMs < 0 ? NULL : &tv);
    if (r < 0) {
        return errno == EINTR ? -2 : -1;
    }
    return r > 0;
}

int ConnOpen(Conn *c, const char *key, Kind kind, int a, int b, const char *name, Config *cfg,
             int seconds, const char *prog) {
    memset(c, 0, sizeof(*c));
    c->coord = c->mine = -1;
    char dir[48], coordPath[64];
    ProtoDir(key, dir, sizeof(dir));
    ProtoCoordPath(key, coordPath, sizeof(coordPath));

    // общий канал создаёт координатор: ждём, пока он появится
    for (int tries = 0; (c->coord = open(coordPath, O_WRONLY | O_NONBLOCK)) < 0; ++tries) {
        if (tries == 0) {
            dprintf(2, "%s: waiting for the coordinator (key '%s')...\n", prog, key);
        }
        if (gotInterrupt || tries >= seconds * 1000 / 200) {
            dprintf(2, "%s: cannot open %s: %s\n", prog, coordPath, strerror(errno));
            return -1;
        }
        SleepMs(200);
    }
    // дальше пишем блокирующе: сообщение маленькое, частичной записи не бывает
    fcntl(c->coord, F_SETFL, fcntl(c->coord, F_GETFL) & ~O_NONBLOCK);

    snprintf(c->path, sizeof(c->path), "%s/p%d", dir, (int)getpid());
    if (mkfifo(c->path, 0600) != 0) {
        dprintf(2, "%s: mkfifo %s: %s\n", prog, c->path, strerror(errno));
        ConnClose(c);
        return -1;
    }
    // O_NONBLOCK: открыть на чтение, не дожидаясь, пока координатор откроет его на запись
    c->mine = open(c->path, O_RDONLY | O_NONBLOCK);
    Msg hello = {M_HELLO, kind, (int)getpid(), a, b, ""};
    snprintf(hello.name, sizeof(hello.name), "%s", name ? name : "");
    if (c->mine < 0 || WriteAll(c->coord, &hello, sizeof(hello)) != 0) {
        dprintf(2, "%s: cannot register: %s\n", prog, strerror(errno));
        ConnClose(c);
        return -1;
    }

    // пока координатор не открыл наш канал, read возвращает 0 - ждём первое сообщение
    Welcome w;
    long got = 0;
    for (int waited = 0; got <= 0; waited += POLL_MS) {
        got = read(c->mine, &w, sizeof(w));
        if (got > 0) {
            break;
        }
        if (waited > seconds * 1000 || gotInterrupt) {
            dprintf(2, "%s: the coordinator did not answer\n", prog);
            ConnClose(c);
            return -1;
        }
        SleepMs(POLL_MS);
    }
    fcntl(c->mine, F_SETFL, fcntl(c->mine, F_GETFL) & ~O_NONBLOCK);
    if (got < (long)sizeof(w) && ReadAll(c->mine, (char *)&w + got, sizeof(w) - (size_t)got) <= 0) {
        got = -1;
    }
    if (got < 0 || w.head.type != M_WELCOME) {
        dprintf(2, "%s: registration refused (%s)\n", prog,
                got < 0 ? "broken message" : "post or car source already connected");
        ConnClose(c);
        return -1;
    }
    *cfg = w.cfg;
    return 0;
}

int ConnSend(Conn *c, Kind kind, int type, int a, int b) {
    Msg m = {type, kind, (int)getpid(), a, b, ""};
    return WriteAll(c->coord, &m, sizeof(m));
}

int ConnRecv(Conn *c, Msg *m, int timeoutMs) {
    int r = WaitReadable(c->mine, timeoutMs);
    if (r <= 0) {
        return r == 0 ? 0 : r;
    }
    long got = ReadAll(c->mine, m, sizeof(*m));
    if (got > 0) {
        return 1;
    }
    if (got < 0 && errno == EINTR) {
        return -2;
    }
    return -1;  // конец файла: координатор закрыл канал или завершился
}

void ConnClose(Conn *c) {
    if (c->mine >= 0) {
        close(c->mine);
    }
    if (c->coord >= 0) {
        close(c->coord);
    }
    if (c->path[0]) {
        unlink(c->path);
    }
    c->mine = c->coord = -1;
    c->path[0] = '\0';
}

int ProtoOpenSems(const char *key, const Config *cfg, sem_t **queue, sem_t **open) {
    char name[40];
    for (int s = 0; s < cfg->stages; ++s) {
        ProtoSemName(key, s, name, sizeof(name));
        queue[s] = sem_open(name, 0);
        if (queue[s] == SEM_FAILED) {
            return -1;
        }
    }
    ProtoSemName(key, -1, name, sizeof(name));
    *open = sem_open(name, 0);
    return *open == SEM_FAILED ? -1 : 0;
}
