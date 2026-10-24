/**
 * @file view_live.c
 * @brief Живая схема: шапка, стадии с очередью и постами, счётчики, панель потоков, события.
 *
 * Оформление перенесено из ИЗ1 (ACOS-1/carwash/src/view_live.c). Отличия: кадр строится по
 * снимку, а не по живой линии; у поста ёмкости K - K полосок; добавлена панель потоков.
 */

#include "view_live.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "sync.h"

#define SHOWN      6   // сколько последних событий показывать внизу
#define KEPT       64  // сколько событий помнить: кадр показывает те, что не позже его минуты
#define CELL       7   // ширина полоски поста
#define DAY_WIDTH  30  // ширина полоски рабочего дня
#define TEXT_WIDTH 100

#define RESET  "\033[0m"
#define DIM    "\033[2m"
#define BOLD   "\033[1m"
#define RED    "\033[1;31m"
#define GREEN  "\033[32m"
#define YELLOW "\033[33m"
#define EOL    "\033[K\n"  // стереть хвост старой строки

// номер машины красим цветом её программы
static const char *PROGRAM_COLOR[] = {"\033[1;36m", "\033[1;33m", "\033[1;35m",
                                      "\033[1;34m", "\033[1;32m", "\033[1;37m"};

typedef struct {
    EventType type;
    int time;
    char clock[16];
    char text[TEXT_WIDTH];
} Recent;

static int out = -1;
static const Config *conf = NULL;
static Mutex lock;  // RANK_OUT: берётся внутри подписчика шины (RANK_BUS)
static Recent recent[KEPT] GUARDED_BY(lock);
static int recentCount GUARDED_BY(lock) = 0;
static int recentHead GUARDED_BY(lock) = 0;

// кадр собираем в памяти и выводим одним write(), иначе экран мерцает; рисует один поток
static char frame[32768];
static int frameLen = 0;

void ViewLiveInit(int fd, const Config *cfg) {
    out = fd;
    conf = cfg;
    MutexInit(&lock, RANK_OUT, "live view");
    MutexLock(&lock);
    recentCount = recentHead = 0;
    MutexUnlock(&lock);
    dprintf(out, "\033[2J\033[?25l");  // очистить экран, спрятать курсор
}

void ViewLiveEnd(void) {
    if (out >= 0) {
        dprintf(out, RESET "\033[?25h");
    }
}

static void Put(const char *fmt, ...) {
    if (frameLen >= (int)sizeof(frame) - 1) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(frame + frameLen, sizeof(frame) - (size_t)frameLen, fmt, args);
    va_end(args);
    if (n > 0) {
        frameLen += n;
        if (frameLen >= (int)sizeof(frame)) {
            frameLen = (int)sizeof(frame) - 1;
        }
    }
}

static void Repeat(const char *glyph, int count) {
    for (int i = 0; i < count; ++i) {
        Put("%s", glyph);
    }
}

static void PutCar(int car, int program, int width) {
    Put("%s%0*d" RESET, PROGRAM_COLOR[program % MAX_PROGRAMS], width, car + 1);
}

/// Пустое место под номер машины: " ·" или "  ·"
static void PutEmpty(int width) {
    Put("%*s" DIM "·" RESET, width - 1, "");
}

/// Одно место поста: номер машины и полоска, которая заполняется по ходу мойки. Словами пишем
/// только особые случаи: машина ждёт, место забронировано или пост в ремонте.
static void PutSeat(const Snapshot *s, int st, int p, int k, int width) {
    const Seat *seat = &s->seat[st][p][k];
    int repair = s->repair[st][p];
    if (seat->car >= 0) {
        PutCar(seat->car, s->seatProg[st][p][k], width);
    } else {
        Put("%*s", width, "");
    }
    Put(" ");
    if (repair > 0) {
        if (k == 0) {
            Put(RED "fix %2dm" RESET, repair);
        } else {
            Put("%*s", CELL, "");
        }
    } else if (seat->car < 0) {
        Put(DIM "free   " RESET);
    } else if (seat->left < 0) {
        Put(DIM "booked " RESET);
    } else if (seat->left == 0) {
        Put(YELLOW "waiting" RESET);
    } else {
        int done = seat->duration > 0 ? CELL * (seat->duration - seat->left) / seat->duration : 0;
        Put(GREEN);
        Repeat("█", done);
        Put(RESET DIM);
        Repeat("░", CELL - done);
        Put(RESET);
    }
    Put("   ");
}

/// Очередь: машины и свободные места, выровнено по самой длинной очереди
static void PutQueue(const Snapshot *s, int st, int width, int columnWidth) {
    int cap = conf->stage[st].queue;
    int slots = cap > s->queued[st] ? cap : s->queued[st];
    for (int i = 0; i < slots; ++i) {
        if (i < s->queued[st]) {
            PutCar(s->queue[st][i], s->queueProg[st][i], width);
        } else {
            PutEmpty(width);
        }
        Put(" ");
    }
    Put("%*s", columnWidth - (slots * (width + 1)), "");
}

/// Шапка: время, полоска рабочего дня, что сейчас с линией и цвета программ
static void PutHeader(const Snapshot *s) {
    char clock[16];
    EventClock(s->now, clock, sizeof(clock));
    Put(BOLD " CAR WASH " RESET " %s  ", clock);

    if (conf->day > 0) {
        int done = s->now >= conf->day ? DAY_WIDTH : DAY_WIDTH * s->now / conf->day;
        Repeat("━", done);
        Put(DIM);
        Repeat("─", DAY_WIDTH - done);
        Put(RESET);
    } else {
        Put(DIM "no closing time" RESET);
    }

    switch ((LineStatus)s->status) {
    case LINE_STALLED: Put(RED "  stalled" RESET); break;
    case LINE_FINISHED: Put(GREEN "  all cars washed" RESET); break;
    case LINE_INTERRUPTED: Put(YELLOW "  interrupted" RESET); break;
    case LINE_NO_RESOURCES: Put(RED "  out of resources" RESET); break;
    case LINE_RUNNING:
    default: Put(s->open ? GREEN "  entrance open" RESET : YELLOW "  entrance closed" RESET);
    }
    Put(EOL);

    Put(" ");
    for (int i = 0; i < conf->programs; ++i) {
        Put("%s●" RESET DIM " %s   " RESET, PROGRAM_COLOR[i % MAX_PROGRAMS], conf->program[i].name);
    }
    Put(EOL EOL);
}

/// "name N (a, b)": счёт потоков вида; при одном потоке без числа в скобках
static void PutKind(const char *name, const Snapshot *s, ThreadKind kind) {
    static const char *const PHASE[PH_COUNT] = {"sleeping", "washing",   "in queue",
                                                "blocked",  "in repair", "active"};
    int total = s->threads[kind];
    Put("%s %d", name, total);
    const char *sep = " (";
    for (int ph = 0; ph < PH_COUNT; ++ph) {
        int n = s->phase[kind][ph];
        if (n == 0) {
            continue;
        }
        if (total == 1) {
            Put("%s%s", sep, PHASE[ph]);
        } else {
            Put("%s%d %s", sep, n, PHASE[ph]);
        }
        sep = ", ";
    }
    if (total > 0) {
        Put(")");
    }
}

/// Где стоит машина на постах: стадия и пост с нуля или -1
static int FindOnPost(const Snapshot *s, int car, int *post) {
    for (int st = 0; st < conf->stages; ++st) {
        for (int p = 0; p < conf->stage[st].posts; ++p) {
            for (int k = 0; k < MAX_CAPACITY; ++k) {
                if (s->seat[st][p][k].car == car) {
                    *post = p;
                    return st;
                }
            }
        }
    }
    return -1;
}

static const char *StageName(int st) {
    return st >= 0 && st < conf->stages ? conf->stage[st].name : "?";
}

/// Панель потоков: сколько потоков каждого вида и кто чего ждёт
static void PutPanel(const Snapshot *s) {
    Put(BOLD " THREADS" RESET "  ");
    PutKind("coordinator", s, TH_COORD);
    Put(DIM " · " RESET);
    PutKind("posts", s, TH_POST);
    Put(DIM " · " RESET);
    PutKind("cars", s, TH_CAR);
    Put(DIM " · " RESET "parks %ld" EOL, s->parks);
    for (int i = 0; i < s->panelCars; ++i) {
        const PanelCar *c = &s->panel[i];
        int post = 0;
        int at = FindOnPost(s, c->car, &post);
        Put("   car #%d ", c->car + 1);
        if (c->why == WAIT_ROOM) {
            if (at >= 0) {
                Put(YELLOW "blocked" RESET " on %s post %d", StageName(at), post + 1);
            } else {
                Put(YELLOW "blocked" RESET);
            }
            Put(" -> room in %s", StageName(c->stage));
        } else {
            Put("waits in %s queue -> a post", StageName(c->stage));
        }
        if (c->sysId > 0) {
            Put(DIM "   [tid %ld]" RESET, c->sysId);
        }
        Put(EOL);
    }
    int more = s->phase[TH_CAR][PH_ROOM] + s->phase[TH_CAR][PH_QUEUE] - s->panelCars;
    if (more > 0) {
        Put(DIM "   ... and %d more waiting" RESET EOL, more);
    }
}

/// Счётчики, аудитор, панель потоков и последние события
static void PutFooter(const Snapshot *s) {
    Put(" washed %d" DIM " · " RESET "inside %d" DIM " · " RESET "turned away %d" DIM " · " RESET
        "breakdowns %d   ",
        s->served, s->inside, s->rejected, s->failures);
    if (s->violations == 0) {
        Put(GREEN "✓" RESET DIM " %ld checks ok" RESET EOL, s->checks);
    } else {
        Put(RED "✗ %ld violations" RESET EOL, s->violations);
    }
    Put(EOL);
    PutPanel(s);
    Put(EOL);

    Recent shown[SHOWN];
    int count = 0;
    MutexLock(&lock);
    for (int i = recentCount - 1; i >= 0 && count < SHOWN; --i) {
        const Recent *r = &recent[(recentHead - recentCount + i + KEPT) % KEPT];
        if (r->time <= s->now) {
            shown[SHOWN - 1 - count++] = *r;  // с конца: новые события в конце списка
        }
    }
    MutexUnlock(&lock);
    for (int i = SHOWN - count; i < SHOWN; ++i) {
        Put(DIM " %s " RESET "%s%s" RESET EOL, shown[i].clock, EventColor(shown[i].type),
            shown[i].text);
    }
}

void ViewLiveDraw(const Snapshot *s) {
    int width = s->arrived >= 99 ? 3 : 2;  // сколько цифр в номере машины
    int columnWidth = 0;
    for (int st = 0; st < conf->stages; ++st) {
        int w = conf->stage[st].queue * (width + 1);
        columnWidth = w > columnWidth ? w : columnWidth;
    }
    columnWidth += 2;

    frameLen = 0;
    Put("\033[H");
    PutHeader(s);
    for (int st = 0; st < conf->stages; ++st) {
        Put(" %-9s ", conf->stage[st].name);
        PutQueue(s, st, width, columnWidth);
        if (conf->stage[st].posts == 0) {
            Put(RED "no posts" RESET);
        }
        int cap = conf->stage[st].capacity > 0 ? conf->stage[st].capacity : 1;
        for (int p = 0; p < conf->stage[st].posts; ++p) {
            for (int k = 0; k < cap; ++k) {
                PutSeat(s, st, p, k, width);
            }
        }
        Put(EOL);
    }
    Put(EOL);
    PutFooter(s);
    Put("\033[J");  // стереть всё, что ниже
    if (out >= 0 && write(out, frame, (size_t)frameLen) < 0) {
        return;  // терминал закрыли - просто пропускаем кадр
    }
}

void ViewLiveListener(const Event *e) {
    char text[200];
    if (EventDescribe(e, text, sizeof(text)) == 0) {
        return;
    }
    MutexLock(&lock);
    Recent *r = &recent[recentHead];
    r->type = e->type;
    r->time = e->time;
    EventClock(e->time, r->clock, sizeof(r->clock));
    snprintf(r->text, sizeof(r->text), "%.*s", (int)sizeof(r->text) - 1, text);
    recentHead = (recentHead + 1) % KEPT;
    if (recentCount < KEPT) {
        ++recentCount;
    }
    MutexUnlock(&lock);
}
