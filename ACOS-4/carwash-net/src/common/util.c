#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DAY_START (8 * 60)  // мойка открывается в 08:00

volatile sig_atomic_t gotInterrupt = 0;
volatile sig_atomic_t gotStatus = 0;
volatile sig_atomic_t gotClose = 0;

static int journal = -1;
static int quietScreen = 0;
static int color = 0;
static OutHook hook = NULL;

static void OnSignal(int sig) {
    if (sig == SIGUSR1) {
        gotStatus = 1;
    } else if (sig == SIGUSR2) {
        gotClose = 1;
    } else {
        gotInterrupt = 1;
    }
}

void SignalsInstall(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = OnSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;  // без SA_RESTART: ожидание прерывается, и флаг проверяется сразу
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    // запись в закрытый сокет должна вернуть EPIPE, а не убить процесс
    signal(SIGPIPE, SIG_IGN);
}

long long NowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int ModelMinute(long long startNs, int delayMs) {
    long long ms = (NowNs() - startNs) / 1000000LL;
    return ms < 0 ? 0 : (int)(ms / delayMs);
}

void FormatClock(int minute, char *buf, int size) {
    int t = DAY_START + minute;
    int day = t / (24 * 60) + 1;
    if (day == 1) {
        snprintf(buf, size, "%02d:%02d", t / 60, t % 60);
    } else {
        snprintf(buf, size, "d%d %02d:%02d", day, (t / 60) % 24, t % 60);
    }
}

int SleepMs(int ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    return nanosleep(&ts, NULL) == 0 ? 0 : -1;
}

int WriteAll(int fd, const void *buf, size_t n) {
    const char *p = buf;
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

long ReadAll(int fd, void *buf, size_t n) {
    char *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r == 0) {
            return got == 0 ? 0 : -1;  // конец посреди сообщения - ошибка
        }
        if (r < 0) {
            if (errno == EINTR && got > 0) {
                continue;  // половину сообщения уже прочли - дочитываем
            }
            return -1;
        }
        got += (size_t)r;
    }
    return (long)got;
}

void OutInit(int journalFd, int quiet) {
    journal = journalFd;
    quietScreen = quiet;
    color = isatty(1);
}

void OutSetHook(OutHook fn) {
    hook = fn;
}

int OutJournal(void) {
    return journal;
}

void OutEvent(int minute, const char *who, int pid, const char *col, const char *fmt, ...) {
    char clock[16], text[256], line[400];
    FormatClock(minute, clock, sizeof(clock));
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);

    int n = snprintf(line, sizeof(line), "[%s] %-12s %6d | %s\n", clock, who, pid, text);
    if (n >= (int)sizeof(line)) {
        n = (int)sizeof(line) - 1;
    }
    if (journal >= 0) {
        WriteAll(journal, line, (size_t)n);
    }
    if (hook) {
        line[n - 1] = '\0';
        hook(line, col);
        line[n - 1] = '\n';
    }
    if (!quietScreen) {
        if (color && col[0]) {
            char painted[420];
            int m = snprintf(painted, sizeof(painted), "%s%.*s\033[0m\n", col, n - 1, line);
            WriteAll(1, painted, (size_t)(m < (int)sizeof(painted) ? m : n));
        } else {
            WriteAll(1, line, (size_t)n);
        }
    }
}

void OutLine(const char *fmt, ...) {
    char line[400];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n >= (int)sizeof(line) - 1) {
        n = (int)sizeof(line) - 2;
    }
    if (hook) {
        line[n] = '\0';
        hook(line, "");
    }
    line[n++] = '\n';
    WriteAll(1, line, (size_t)n);
    if (journal >= 0) {
        WriteAll(journal, line, (size_t)n);
    }
}
