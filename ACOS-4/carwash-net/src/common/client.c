#include "client.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "util.h"

int SessionOpen(Session *s, const char *name, const char *host, int port, int seconds,
                const char *hello, char *welcome, int size) {
    memset(s, 0, sizeof(*s));
    s->name = name;
    s->delayMs = 100;
    s->baseNs = NowNs();
    char err[200];
    int fd = NetConnect(host, port, seconds, err, sizeof(err));
    if (fd < 0) {
        dprintf(2, "%s: %s\n", name, err);
        return -1;
    }
    LineInit(&s->io, fd);
    if (NetSend(fd, "%s", hello) != 0) {
        dprintf(2, "%s: the server closed the connection\n", name);
        LineClose(&s->io);
        return -1;
    }
    int r = LineWait(&s->io, welcome, size, 5000);
    if (r == 1 && strncmp(welcome, "WELCOME ", 8) == 0) {
        return 0;
    }
    if (r == 1 && strncmp(welcome, "REJECT ", 7) == 0) {
        dprintf(2, "%s: the server refused: %s\n", name, welcome + 7);
    } else if (r == 1 && strncmp(welcome, "STOP ", 5) == 0) {
        dprintf(2, "%s: the server is stopping\n", name);
    } else {
        dprintf(2, "%s: no answer from the server\n", name);
    }
    LineClose(&s->io);
    return -1;
}

int SessionNow(const Session *s) {
    long long ms = (NowNs() - s->baseNs) / 1000000LL;
    return s->baseMinute + (ms < 0 ? 0 : (int)(ms / s->delayMs));
}

void SessionSync(Session *s, int minute) {
    s->baseMinute = minute;
    s->baseNs = NowNs();
}

int SessionRecv(Session *s, char *line, int size, int timeoutMs) {
    long long deadline = timeoutMs >= 0 ? NowNs() + (long long)timeoutMs * 1000000LL : 0;
    for (;;) {
        // флаг - до ожидания: сигнал мог прийти вне poll, и тогда EINTR не будет
        if (gotInterrupt) {
            return -3;
        }
        int left = -1;
        if (timeoutMs >= 0) {
            left = (int)((deadline - NowNs()) / 1000000LL);
            if (left < 0) {
                left = 0;
            }
        }
        int r = LineWait(&s->io, line, size, left);
        if (r != -2) {
            return r;
        }
    }
}

void SessionBye(Session *s) {
    if (s->io.fd >= 0) {
        NetSend(s->io.fd, "BYE");
    }
    LineClose(&s->io);
}

int SessionStopped(const Session *s, const char *line) {
    int code = 0, off = 0;
    sscanf(line, "STOP %d %n", &code, &off);
    dprintf(1, "%s: the server stopped the line (%s, server exit code %d), exiting\n", s->name,
            off > 0 ? line + off : "?", code);
    return 0;
}
