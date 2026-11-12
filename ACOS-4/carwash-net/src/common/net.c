#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "config.h"
#include "soft_assert.h"
#include "util.h"

int NetParsePort(const char *s, int *port) {
    if (ParseInt(s, port) != 0 || *port < 1 || *port > 65535) {
        dprintf(2, "bad port '%s': expected a number 1..65535\n", s);
        return -1;
    }
    return 0;
}

/// Сообщения - короткие строки: без алгоритма Нейгла они уходят сразу, а не пачками.
/// Таймаут записи: клиент, который перестал читать, не может надолго остановить сервер.
static void Tune(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

int NetListen(const char *ip, int port, char *err, int errSize) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        snprintf(err, errSize, "bad IPv4 address '%s'", ip);
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        snprintf(err, errSize, "socket: %s", strerror(errno));
        return -1;
    }
    // перезапуск сервера сразу после остановки: порт в TIME_WAIT не должен мешать bind
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        snprintf(err, errSize, "bind %s:%d: %s", ip, port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 16) != 0) {
        snprintf(err, errSize, "listen: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

int NetConnect(const char *host, int port, int seconds, char *err, int errSize) {
    char service[8];
    snprintf(service, sizeof(service), "%d", port);
    struct addrinfo hints, *list = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, service, &hints, &list);
    if (rc != 0) {
        snprintf(err, errSize, "cannot resolve '%s': %s", host, gai_strerror(rc));
        return -1;
    }
    long long deadline = NowNs() + ((long long)seconds * 1000000000LL);
    int lastErr = 0;
    for (;;) {
        for (struct addrinfo *a = list; a; a = a->ai_next) {
            int fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (fd < 0) {
                lastErr = errno;
                continue;
            }
            if (connect(fd, a->ai_addr, a->ai_addrlen) == 0) {
                freeaddrinfo(list);
                Tune(fd);
                return fd;
            }
            lastErr = errno;
            close(fd);
        }
        if (gotInterrupt || NowNs() >= deadline) {
            break;
        }
        SleepMs(300);
    }
    freeaddrinfo(list);
    snprintf(err, errSize, "cannot connect to %s:%d: %s", host, port,
             gotInterrupt ? "interrupted" : strerror(lastErr));
    return -1;
}

void NetPeerName(int fd, char *buf, int size) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    char ip[INET_ADDRSTRLEN] = "?";
    if (getpeername(fd, (struct sockaddr *)&addr, &len) == 0) {
        inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip));
        snprintf(buf, size, "%s:%d", ip, ntohs(addr.sin_port));
    } else {
        snprintf(buf, size, "?");
    }
}

void LineInit(LineConn *c, int fd) {
    c->fd = fd;
    c->len = 0;
    if (fd >= 0) {
        Tune(fd);
    }
}

void LineClose(LineConn *c) {
    if (c->fd >= 0) {
        close(c->fd);
    }
    c->fd = -1;
    c->len = 0;
}

int LineFill(LineConn *c) {
    if (c->len >= NET_BUF) {
        return -1;  // буфер полон, а целой строки в нём нет - собеседник шлёт мусор
    }
    ssize_t r = read(c->fd, c->buf + c->len, (size_t)(NET_BUF - c->len));
    if (r < 0) {
        return errno == EINTR ? -2 : -1;
    }
    c->len += (int)r;
    return (int)r;
}

int LineNext(LineConn *c, char *out, int size) {
    char *nl = memchr(c->buf, '\n', (size_t)c->len);
    if (!nl) {
        return 0;
    }
    int n = (int)(nl - c->buf);
    int copy = n < size - 1 ? n : size - 1;
    memcpy(out, c->buf, (size_t)copy);
    out[copy] = '\0';
    if (copy > 0 && out[copy - 1] == '\r') {
        out[copy - 1] = '\0';  // строки, набранные руками в nc или telnet
    }
    c->len -= n + 1;
    memmove(c->buf, nl + 1, (size_t)c->len);
    return 1;
}

int LineWait(LineConn *c, char *out, int size, int timeoutMs) {
    long long deadline = timeoutMs >= 0 ? NowNs() + ((long long)timeoutMs * 1000000LL) : 0;
    for (;;) {
        if (LineNext(c, out, size)) {
            return 1;
        }
        int left = -1;
        if (timeoutMs >= 0) {
            left = (int)((deadline - NowNs()) / 1000000LL);
            if (left <= 0) {
                return 0;
            }
        }
        struct pollfd p = {c->fd, POLLIN, 0};
        int r = poll(&p, 1, left);
        if (r < 0) {
            return errno == EINTR ? -2 : -1;
        }
        if (r == 0) {
            return 0;
        }
        r = LineFill(c);
        if (r == -2) {
            return -2;
        }
        if (r <= 0) {
            return -1;
        }
    }
}

int NetSend(int fd, const char *fmt, ...) {
    if (fd < 0) {
        return -1;
    }
    char line[LINE_MAX_LEN + 2];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, LINE_MAX_LEN, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return -1;
    }
    if (!SOFT_ASSERT(n < LINE_MAX_LEN, "protocol line is too long, it is cut")) {
        n = LINE_MAX_LEN - 1;
    }
    line[n++] = '\n';
    return WriteAll(fd, line, (size_t)n);
}
