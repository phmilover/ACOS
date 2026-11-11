/*
 * Клиент-наблюдатель. Показывает работу всей линии: очереди и посты каждой стадии, машины на
 * них, кто из клиентов в сети, счётчики дня и ленту последних событий. Данные приходят от
 * сервера: при подключении - конфигурация линии и последние события, дальше - каждое событие
 * (EV) и снимки состояния (SNAP ... END) несколько раз в секунду.
 *
 * Наблюдателей может быть сколько угодно, на любых компьютерах; подключение и отключение
 * наблюдателя на работу линии не влияет. Клавиша q или Ctrl+C - выйти.
 *
 * Два режима вывода: панель на весь экран терминала (по умолчанию) и лента строк (-p, или
 * если вывод не в терминал - например, в файл).
 */
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "client.h"
#include "config.h"
#include "util.h"

#define MAX_EVENTS  64  ///< сколько последних событий помнит панель
#define MAX_SUMMARY 40  ///< строк итогов после остановки

/// Цвета панели.
#define RESET  "\033[0m"
#define BOLD   "\033[1m"
#define DIM    "\033[2m"
#define RED    "\033[1;31m"
#define GREEN  "\033[32m"
#define YELLOW "\033[33m"
#define BLUE   "\033[34m"
#define CYAN   "\033[36m"
#define MAG    "\033[35m"
#define GREY   "\033[90m"

typedef struct {
    char name[NAME_LEN + 4];
    int posts, queue, tmin, tmax;
} StageView;

typedef struct {
    char state[12];  ///< off | idle | busy | broken
    int online, car, program, running, wait, ops, fails;
} PostView;

typedef struct {
    int len;
    int car[MAX_QUEUE], program[MAX_QUEUE];
} QueueView;

/// Всё, что наблюдатель знает о линии.
typedef struct {
    int stages, programs, day, cars, delayMs;
    char rule[16], server[48];
    StageView stage[MAX_STAGES];
    char program[MAX_PROGRAMS][NAME_LEN + 4];

    int minute, open, closed, active, arrived, accepted, finished, turned, refused;
    int source, viewers, stopping, violations;
    QueueView queue[MAX_STAGES];
    PostView post[MAX_STAGES][MAX_POSTS];
    int haveSnap;
} Line;

static Line ln;
static Session ses;
static int plain = 0;  ///< лента строк вместо панели
static int color = 1;
static struct termios savedTty;
static int ttySaved = 0;

static char events[MAX_EVENTS][LINE_MAX_LEN];
static char eventColor[MAX_EVENTS];
static int eventCount = 0, eventHead = 0;
static char summary[MAX_SUMMARY][LINE_MAX_LEN];
static int summaryCount = 0;
static int stoppingSeen = 0;
static char stopLine[LINE_MAX_LEN] = "STOP 0 ?";

/* ---------------------------------------------------------------- терминал */

static void TerminalRestore(void) {
    if (!plain) {
        dprintf(1, "\033[?25h\033[?1049l");  // курсор и обычный экран обратно
    }
    if (ttySaved) {
        tcsetattr(0, TCSANOW, &savedTty);
        ttySaved = 0;
    }
}

/// Панель: отдельный экран терминала, без курсора; клавиши - без Enter и без эха.
static void TerminalSetup(void) {
    if (isatty(0) && tcgetattr(0, &savedTty) == 0) {
        struct termios raw = savedTty;
        raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &raw);
        ttySaved = 1;
    }
    dprintf(1, "\033[?1049h\033[?25l");
}

static void TermSize(int *cols, int *rows) {
    struct winsize ws;
    *cols = 100;
    *rows = 40;
    if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    }
    // размер можно задать и явно - для записи кадров через script(1)
    const char *c = getenv("COLUMNS"), *r = getenv("LINES");
    long v = c ? strtol(c, NULL, 10) : 0;
    if (v > 20 && v < 1000) {
        *cols = (int)v;
    }
    v = r ? strtol(r, NULL, 10) : 0;
    if (v > 10 && v < 1000) {
        *rows = (int)v;
    }
}

/* ---------------------------------------------------------------- кадр панели */

static char frame[64 * 1024];
static int frameLen;
static int frameCols;

/// Ширина строки на экране: escape-последовательности не видны, UTF-8 - один символ.
static int Visible(const char *s, int bytes, int maxCols, int *cut) {
    int cols = 0, i = 0;
    while (i < bytes && s[i]) {
        if (s[i] == '\033') {
            while (i < bytes && s[i] && s[i] != 'm') {
                ++i;
            }
            ++i;
            continue;
        }
        if (((unsigned char)s[i] & 0xC0U) != 0x80U) {
            if (cols == maxCols) {
                break;
            }
            ++cols;
        }
        ++i;
    }
    if (cut) {
        *cut = i;
    }
    return cols;
}

/// Добавляет в кадр строку, обрезанную по ширине терминала.
static void Row(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void Row(const char *fmt, ...) {
    char text[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n >= (int)sizeof(text)) {
        n = (int)sizeof(text) - 1;
    }
    int cut;
    Visible(text, n, frameCols, &cut);
    if (frameLen + cut + 16 < (int)sizeof(frame)) {
        memcpy(frame + frameLen, text, (size_t)cut);
        frameLen += cut;
        frameLen += snprintf(frame + frameLen, sizeof(frame) - frameLen, RESET "\033[K\n");
    }
}

/// Дополняет строку пробелами до @p width видимых символов.
static void PadTo(char *buf, int size, int width) {
    int n = (int)strlen(buf);
    int cols = Visible(buf, n, INT_MAX, NULL);
    while (cols < width && n < size - 1) {
        buf[n++] = ' ';
        ++cols;
    }
    buf[n] = '\0';
}

/// Полоса из @p width клеток, @p full из них заполнены.
static void Bar(char *buf, int size, int full, int width, const char *on, const char *off) {
    int n = 0;
    buf[0] = '\0';
    for (int i = 0; i < width && n < size - 8; ++i) {
        n += snprintf(buf + n, size - n, "%s", i < full ? on : off);
    }
}

static const char *EventColor(char l) {
    switch (l) {
    case 'c': return CYAN;
    case 'w': return YELLOW;
    case 'f': return RED;
    case 'x': return "\033[1;32m";
    case 'd': return GREEN;
    case 'k': return "\033[1;35m";
    default: return "";
    }
}

/// Строка поста: значок, машина и что с ней происходит.
static void PostText(int s, int p, char *buf, int size) {
    const PostView *pv = &ln.post[s][p];
    char car[64] = "";
    if (pv->car > 0) {
        snprintf(car, sizeof(car), "#%-3d %-9s", pv->car,
                 pv->program >= 0 && pv->program < ln.programs ? ln.program[pv->program] : "?");
    }
    if (!pv->online) {
        if (pv->car > 0) {
            snprintf(buf, size, GREY "○ offline  " RESET "%s" GREY "%s" RESET, car,
                     pv->running ? "interrupted, redo on return" : "waits on the post");
        } else {
            snprintf(buf, size, GREY "○ offline  client is not connected" RESET);
        }
    } else if (strcmp(pv->state, "broken") == 0) {
        snprintf(buf, size, RED "× BROKEN   under repair" RESET);
    } else if (pv->car > 0 && pv->running) {
        snprintf(buf, size, GREEN "► washing  " RESET BOLD "%s" RESET, car);
    } else if (pv->car > 0) {
        const char *next = pv->wait >= 0 && pv->wait < ln.stages ? ln.stage[pv->wait].name : "?";
        snprintf(buf, size, YELLOW "‖ waiting  " RESET "%s" YELLOW "done, waits for %s" RESET, car,
                 next);
    } else if (strcmp(pv->state, "busy") == 0) {
        snprintf(buf, size, CYAN "· getting ready" RESET);
    } else {
        snprintf(buf, size, DIM "· idle" RESET);
    }
}

static void Render(void) {
    int rows;
    TermSize(&frameCols, &rows);
    frameLen = snprintf(frame, sizeof(frame), "\033[H");
    char clock[16], end[16], bar[256];
    FormatClock(ln.minute, clock, sizeof(clock));

    const char *door = !ln.open      ? YELLOW "◌ waiting for clients" RESET
                       : ln.stopping ? MAG "■ line stopped" RESET
                       : ln.closed   ? YELLOW "● entrance closed" RESET
                                     : GREEN "● entrance open" RESET;
    Row(BOLD " CAR WASH" RESET DIM "  variant 43 · TCP · server %s" RESET, ln.server);
    Row(" " BOLD "%s" RESET "   %s   " DIM "rule %s, %d ms per minute" RESET, clock, door, ln.rule,
        ln.delayMs);
    if (ln.day > 0) {
        int pct = ln.minute * 100 / ln.day;
        if (pct > 100) {
            pct = 100;
        }
        FormatClock(ln.day, end, sizeof(end));
        Bar(bar, sizeof(bar), pct * 30 / 100, 30, "━", DIM "─" RESET);
        Row(" day  %s %s %3d%%  until %s", bar, RESET, pct, end);
    } else {
        Row(" day  " DIM "no closing time, stop with Ctrl+C or SIGUSR2 to the server" RESET);
    }
    Row(DIM "%s" RESET, "─────────────────────────────────────────────────────────────────────────"
                        "───────────────────────────────────");

    int maxQueue = 1;
    for (int s = 0; s < ln.stages; ++s) {
        if (ln.stage[s].queue > maxQueue) {
            maxQueue = ln.stage[s].queue;
        }
    }
    for (int s = 0; s < ln.stages; ++s) {
        const StageView *sv = &ln.stage[s];
        const QueueView *q = &ln.queue[s];
        char cars[256] = "";
        int n = 0;
        for (int i = 0; i < q->len && n < (int)sizeof(cars) - 40; ++i) {
            n += snprintf(
                cars + n, sizeof(cars) - n, "  #%d " DIM "%.3s" RESET, q->car[i],
                q->program[i] >= 0 && q->program[i] < ln.programs ? ln.program[q->program[i]] : "");
        }
        if (sv->queue > 0) {
            const char *on = q->len >= sv->queue ? YELLOW "■" RESET : CYAN "■" RESET;
            Bar(bar, sizeof(bar), q->len, sv->queue, on, DIM "□" RESET);
            PadTo(bar, sizeof(bar), maxQueue);
            Row(" " BOLD "%-9s" RESET " queue  %s %s%2d/%d" RESET "%s", sv->name, bar,
                q->len >= sv->queue ? YELLOW : "", q->len, sv->queue, cars);
        } else {
            Row(" " BOLD "%-9s" RESET DIM " no queue: a car waits on the previous post" RESET,
                sv->name);
        }
        if (sv->posts == 0) {
            Row("           " RED "no posts - programs through this stage are refused" RESET);
        }
        for (int p = 0; p < sv->posts; ++p) {
            char text[300];
            PostText(s, p, text, sizeof(text));
            PadTo(text, sizeof(text), 62);
            const PostView *pv = &ln.post[s][p];
            Row("           post %d  %s" DIM "ops %-3d breaks %d" RESET, p + 1, text, pv->ops,
                pv->fails);
        }
    }
    Row(DIM "%s" RESET, "─────────────────────────────────────────────────────────────────────────"
                        "───────────────────────────────────");
    char arrived[24];
    if (ln.cars > 0) {
        snprintf(arrived, sizeof(arrived), "%d/%d", ln.arrived, ln.cars);
    } else {
        snprintf(arrived, sizeof(arrived), "%d", ln.arrived);
    }
    Row(" cars     arrived " BOLD "%s" RESET " · on the line " BOLD CYAN "%d" RESET
        " · washed " BOLD GREEN "%d" RESET " · turned away " YELLOW "%d" RESET " · refused %d",
        arrived, ln.active, ln.finished, ln.turned, ln.refused);
    int online = 0, total = 0;
    for (int s = 0; s < ln.stages; ++s) {
        for (int p = 0; p < ln.stage[s].posts; ++p) {
            ++total;
            online += ln.post[s][p].online;
        }
    }
    Row(" clients  posts %s%d/%d" RESET " · car source %s · viewers %d%s",
        online == total ? GREEN : YELLOW, online, total,
        ln.source ? GREEN "● online" RESET : GREY "○ offline" RESET, ln.viewers,
        ln.violations ? RED "   INVARIANT VIOLATIONS!" RESET : "");
    Row(DIM "─ events ────────────────────────────────────────────────────────────────────────"
            "───────────────────────────" RESET);

    int used = 0;
    for (const char *c = frame; c < frame + frameLen; ++c) {
        used += *c == '\n';
    }
    int room = rows - used - 2;
    if (room > eventCount) {
        room = eventCount;
    }
    for (int k = eventCount - room; k < eventCount; ++k) {
        int i = (eventHead - eventCount + k + MAX_EVENTS) % MAX_EVENTS;
        Row(" %s%s", EventColor(eventColor[i]), events[i]);
    }
    for (int k = room; k < rows - used - 2; ++k) {
        Row("%s", "");
    }
    Row(DIM " q - quit (the line keeps working)" RESET);
    frameLen += snprintf(frame + frameLen, sizeof(frame) - frameLen, "\033[J");
    WriteAll(1, frame, (size_t)frameLen);
}

/* ---------------------------------------------------------------- сообщения сервера */

static void AddEvent(char l, const char *text) {
    snprintf(events[eventHead], LINE_MAX_LEN, "%s", text);
    eventColor[eventHead] = l;
    eventHead = (eventHead + 1) % MAX_EVENTS;
    if (eventCount < MAX_EVENTS) {
        ++eventCount;
    }
    if (strstr(text, "| stopping:")) {
        stoppingSeen = 1;
    }
    if (stoppingSeen && l == 'n' && summaryCount < MAX_SUMMARY) {
        snprintf(summary[summaryCount++], LINE_MAX_LEN, "%s", text);
    }
    if (plain) {
        const char *c = color ? EventColor(l) : "";
        dprintf(1, "%s%s%s\n", c, text, c[0] ? RESET : "");
    }
}

/// Строка снимка; @return 1, когда снимок закончен (END)
static int SnapLine(const char *line) {
    int s, p, len, off;
    if (sscanf(line, "SNAP %d %d %d %d %d %d %d %d %d %d %d %d %d", &ln.minute, &ln.open,
               &ln.closed, &ln.active, &ln.arrived, &ln.accepted, &ln.finished, &ln.turned,
               &ln.refused, &ln.source, &ln.viewers, &ln.stopping, &ln.violations) == 13) {
        return 0;
    }
    if (sscanf(line, "Q %d %d %n", &s, &len, &off) == 2 && s >= 0 && s < MAX_STAGES) {
        QueueView *q = &ln.queue[s];
        q->len = 0;
        const char *c = line + off;
        int id, prog, used;
        while (q->len < MAX_QUEUE && sscanf(c, "%d:%d %n", &id, &prog, &used) == 2) {
            q->car[q->len] = id;
            q->program[q->len++] = prog;
            c += used;
        }
        return 0;
    }
    PostView pv;
    if (sscanf(line, "P %d %d %11s %d %d %d %d %d %d %d", &s, &p, pv.state, &pv.online, &pv.car,
               &pv.program, &pv.running, &pv.wait, &pv.ops, &pv.fails) == 10 &&
        s >= 0 && s < MAX_STAGES && p >= 0 && p < MAX_POSTS) {
        ln.post[s][p] = pv;
        return 0;
    }
    return strcmp(line, "END") == 0;
}

static void ConfigLine(const char *line) {
    int i, len, off;
    char name[NAME_LEN + 4];
    StageView sv;
    if (sscanf(line, "STAGE %d %15s %d %d %d %d", &i, sv.name, &sv.posts, &sv.queue, &sv.tmin,
               &sv.tmax) == 6 &&
        i >= 0 && i < MAX_STAGES) {
        ln.stage[i] = sv;
    } else if (sscanf(line, "PROGRAM %d %15s %d %n", &i, name, &len, &off) == 3 && i >= 0 &&
               i < MAX_PROGRAMS) {
        snprintf(ln.program[i], sizeof(ln.program[i]), "%s", name);
    }
}

static void Usage(int fd) {
    dprintf(fd, "Usage: cw-view [-p] SERVER PORT\n"
                "  SERVER  server address or name, e.g. 127.0.0.1\n"
                "  PORT    server port\n"
                "  -p      plain event feed instead of the full-screen board\n"
                "Start as many viewers as you like, on any computer; q or Ctrl+C quits.\n");
}

int main(int argc, char **argv) {
    int opt;
    while ((opt = getopt(argc, argv, "ph")) != -1) {
        if (opt == 'p') {
            plain = 1;
        } else {
            Usage(opt == 'h' ? 1 : 2);
            return opt == 'h' ? 0 : 1;
        }
    }
    int port;
    if (argc - optind != 2) {
        Usage(2);
        return 1;
    }
    if (NetParsePort(argv[optind + 1], &port) != 0) {
        return 1;
    }
    if (!isatty(1)) {
        plain = 1;
        color = 0;
    }
    SignalsInstall();
    char hello[48], line[LINE_MAX_LEN];
    snprintf(hello, sizeof(hello), "HELLO view %d", (int)getpid());
    if (SessionOpen(&ses, "cw-view", argv[optind], port, 15, hello, line, sizeof(line)) != 0) {
        return 1;
    }
    if (sscanf(line, "WELCOME view %d %d %d %d %d %15s %47s", &ln.stages, &ln.programs, &ln.day,
               &ln.cars, &ln.delayMs, ln.rule, ln.server) != 7 ||
        ln.stages < 1 || ln.stages > MAX_STAGES || ln.programs > MAX_PROGRAMS) {
        dprintf(2, "cw-view: unexpected answer: %s\n", line);
        return 5;
    }
    if (plain) {
        dprintf(1, "cw-view: connected to %s:%d, the line has %d stages; events follow\n",
                argv[optind], port, ln.stages);
    } else {
        TerminalSetup();
    }

    int code = 0, quit = 0, inSnap = 0, dirty = 1;
    const char *why = "";
    while (!quit) {
        struct pollfd pf[2] = {{ses.io.fd, POLLIN, 0}, {0, POLLIN, 0}};
        int n = plain || !ttySaved ? 1 : 2;
        int r = poll(pf, (nfds_t)n, dirty ? 30 : 1000);
        if (gotInterrupt) {
            break;
        }
        if (r < 0 && errno != EINTR) {
            break;
        }
        if (n == 2 && r > 0 && pf[1].revents != 0) {
            char key;
            while (read(0, &key, 1) == 1) {
                if (key == 'q' || key == 'Q') {
                    quit = 1;
                }
            }
        }
        if (r > 0 && pf[0].revents) {
            int got = LineFill(&ses.io);
            if (got == 0 || got == -1) {
                why = "the server is gone";
                code = 5;
                break;
            }
            while (LineNext(&ses.io, line, sizeof(line))) {
                if (strncmp(line, "EV ", 3) == 0 && line[3] && line[4] == ' ') {
                    AddEvent(line[3], line + 5);
                    dirty = 1;
                } else if (strncmp(line, "SNAP ", 5) == 0 || inSnap) {
                    inSnap = !SnapLine(line);
                    if (!inSnap) {
                        ln.haveSnap = 1;
                        dirty = 1;
                    }
                } else if (strncmp(line, "STOP ", 5) == 0) {
                    why = "stop";
                    quit = 1;
                    snprintf(stopLine, sizeof(stopLine), "%s", line);
                } else {
                    ConfigLine(line);
                }
            }
        }
        if (!plain && dirty && !inSnap && ln.haveSnap) {
            Render();
            dirty = 0;
        }
    }

    if (!plain) {
        if (ln.haveSnap) {
            Render();
        }
        // последний кадр остаётся в обычном экране терминала - видно, чем закончился день
        int keep = frameLen;
        TerminalRestore();
        if (strcmp(why, "stop") == 0 && keep > 0) {
            WriteAll(1, frame + 3, (size_t)(keep - 3 - 3));  // без перехода в начало и \033[J
            for (int i = 0; i < summaryCount; ++i) {
                dprintf(1, "%s\n", summary[i]);
            }
        }
    }
    if (strcmp(why, "stop") == 0) {
        SessionStopped(&ses, stopLine);
    } else if (code == 5) {
        dprintf(2, "cw-view: %s\n", why);
    } else {
        dprintf(1, "cw-view: viewer closed; the line keeps working\n");
    }
    SessionBye(&ses);
    return code;
}
