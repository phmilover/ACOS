/**
 * @file waitgraph.c
 * @brief Граф ожидания и таблица потоков.
 *
 * Сначала снимок: под мьютексом каждой стадии (по одному, не вкладывая) копируются места на
 * постах, очередь и список ожидающих. Затем по снимку, уже без мьютексов, печатается текст
 * или DOT. Таблица потоков копирует данные часов в SimForEach (там мьютекс часов: ничего не
 * печатаем и других мьютексов не берём) и печатает их потом.
 */

#include "waitgraph.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "events.h"
#include "sim.h"

/// Сколько машин может быть на линии: все места постов и очередей.
#define WG_MAX       (MAX_STAGES * ((MAX_POSTS * MAX_CAPACITY) + MAX_QUEUE))
#define HOLDERS_MAX  (MAX_POSTS * MAX_CAPACITY)
#define STAGE_COLUMN 28  // ширина строки о стадии до "<- ROOT CAUSE"
#define TEXT_LEN     160

#define RED   "\033[1;31m"
#define BOLD  "\033[1m"
#define RESET "\033[0m"

/// Что делает машина.
typedef enum {
    WG_WASHING,  ///< идёт операция
    WG_LEAVING,  ///< помыта, места дальше не просила (в покое не бывает)
    WG_REPAIR,   ///< операция замерла: пост сломан
    WG_POST,     ///< стоит в очереди, ждёт пост этой же стадии
    WG_ROOM,     ///< помыта, ждёт места на следующей стадии
} WgKind;

typedef struct {
    int car;          ///< номер машины (с нуля)
    int stage, post;  ///< где стоит; post = -1 - в очереди
    WgKind kind;
    int target;  ///< какую стадию ждёт (WG_POST, WG_ROOM) или -1
    int left;    ///< минут до конца операции или ремонта
} WgCar;

typedef struct {
    int posts, seats, broken, minRepair, queued, queueCap;
    int holder[HOLDERS_MAX];  ///< машины на постах
    int holders;
} WgStage;

typedef struct {
    const Config *cfg;
    int now, n, stuck;
    WgCar car[WG_MAX];
    WgStage st[MAX_STAGES];
    int root;  ///< стадия - корневая причина или -1
    char rootText[TEXT_LEN];
} WgSnap;

static WgCar *FindCar(WgSnap *g, int id) {
    for (int i = 0; i < g->n; ++i) {
        if (g->car[i].car == id) {
            return &g->car[i];
        }
    }
    return NULL;
}

static void AddCar(WgSnap *g, WgCar c) {
    if (g->n < WG_MAX) {
        g->car[g->n++] = c;
    }
}

/// Копирует стадию s под её мьютексом; ожидающих места записывает в waits (машина, стадия).
static void CollectStage(Line *l, WgSnap *g, int s, int waits[][2], int *nWaits) {
    const StageSpec *spec = &l->cfg->stage[s];
    Stage *st = &l->stage[s];
    WgStage *w = &g->st[s];
    MutexLock(&st->m);
    w->posts = spec->posts;
    w->seats = spec->posts * spec->capacity;
    w->queueCap = spec->queue;
    w->queued = st->queued;
    for (int p = 0; p < spec->posts; ++p) {
        const Post *post = &st->post[p];
        if (post->repair > 0) {
            w->broken++;
            if (w->minRepair == 0 || post->repair < w->minRepair) {
                w->minRepair = post->repair;
            }
        }
        for (int k = 0; k < spec->capacity; ++k) {
            const Seat *seat = &post->seat[k];
            if (seat->car < 0 || seat->left < 0) {
                continue;  // свободно или бронь: машина ещё едет (в покое не бывает)
            }
            w->holder[w->holders++] = seat->car;
            WgCar c = {.car = seat->car, .stage = s, .post = p, .target = -1};
            if (seat->left == 0) {
                c.kind = WG_LEAVING;  // станет WG_ROOM, если найдётся в списке ожидающих
            } else if (post->repair > 0) {
                c.kind = WG_REPAIR;
                c.left = post->repair;
            } else {
                c.kind = WG_WASHING;
                c.left = seat->left;
            }
            AddCar(g, c);
        }
    }
    for (int i = 0; i < st->queued; ++i) {
        AddCar(g,
               (WgCar){.car = st->queue[i], .stage = s, .post = -1, .kind = WG_POST, .target = s});
    }
    for (int i = 0; i < st->waiters && *nWaits < WG_MAX; ++i) {
        waits[*nWaits][0] = st->waiting[i]->id;  // номер машины не меняется
        waits[*nWaits][1] = s;
        ++*nWaits;
    }
    MutexUnlock(&st->m);
}

static int ByCar(const void *a, const void *b) {
    return ((const WgCar *)a)->car - ((const WgCar *)b)->car;
}

/// Все ли машины на постах стадии ждут места дальше.
static int AllBlocked(WgSnap *g, const WgStage *w) {
    for (int i = 0; i < w->holders; ++i) {
        const WgCar *c = FindCar(g, w->holder[i]);
        if (!c || c->kind != WG_ROOM) {
            return 0;
        }
    }
    return w->holders > 0;
}

/// Ищет корневую причину среди стадий, которые кто-то ждёт (дальние по линии - первыми).
static void FindRoot(WgSnap *g) {
    const Config *cfg = g->cfg;
    int wanted[MAX_STAGES] = {0};
    for (int i = 0; i < g->n; ++i) {
        if (g->car[i].target >= 0) {
            wanted[g->car[i].target] = 1;
        }
    }
    g->root = -1;
    for (int s = cfg->stages - 1; s >= 0 && g->root < 0; --s) {
        const WgStage *w = &g->st[s];
        if (!wanted[s]) {
            continue;
        }
        if (w->posts == 0) {
            g->root = s;
            snprintf(g->rootText, sizeof g->rootText, "stage %s has no posts", cfg->stage[s].name);
        } else if (w->broken == w->posts) {
            g->root = s;
            snprintf(g->rootText, sizeof g->rootText, "all %s posts are broken (repair %d min)",
                     cfg->stage[s].name, w->minRepair);
        }
    }
    // запасной случай: ни пустой, ни сломанной стадии нет, но стадия забита стоящими машинами
    for (int s = cfg->stages - 1; s >= 0 && g->root < 0; --s) {
        const WgStage *w = &g->st[s];
        if (wanted[s] && w->holders == w->seats && w->queued >= w->queueCap && AllBlocked(g, w)) {
            g->root = s;
            snprintf(g->rootText, sizeof g->rootText, "%s is full of blocked cars",
                     cfg->stage[s].name);
        }
    }
}

/// Снимок линии для графа.
static void Collect(Line *l, WgSnap *g) {
    memset(g, 0, sizeof *g);
    g->cfg = l->cfg;
    g->now = SimNow();
    static _Thread_local int waits[WG_MAX][2];
    int nWaits = 0;
    for (int s = 0; s < l->cfg->stages; ++s) {
        CollectStage(l, g, s, waits, &nWaits);
    }
    for (int i = 0; i < nWaits; ++i) {
        WgCar *c = FindCar(g, waits[i][0]);
        if (c && c->kind == WG_LEAVING) {
            c->kind = WG_ROOM;
            c->target = waits[i][1];
        }
    }
    qsort(g->car, (size_t)g->n, sizeof g->car[0], ByCar);
    for (int i = 0; i < g->n; ++i) {
        g->stuck += g->car[i].kind >= WG_REPAIR;
    }
    FindRoot(g);
}

static void PlaceText(const WgSnap *g, const WgCar *c, char *buf, int size) {
    const char *name = g->cfg->stage[c->stage].name;
    if (c->post >= 0) {
        snprintf(buf, size, "on %s post %d", name, c->post + 1);
    } else {
        snprintf(buf, size, "in %s queue", name);
    }
}

static void WaitText(const WgSnap *g, const WgCar *c, char *buf, int size) {
    const char *target = c->target >= 0 ? g->cfg->stage[c->target].name : "";
    switch (c->kind) {
    case WG_POST: snprintf(buf, size, "waits for a post in %s", target); break;
    case WG_ROOM: snprintf(buf, size, "waits for room in %s", target); break;
    case WG_REPAIR: snprintf(buf, size, "waits for repair (%d min)", c->left); break;
    case WG_WASHING: snprintf(buf, size, "is being washed (%d min left)", c->left); break;
    case WG_LEAVING:
    default: snprintf(buf, size, "is washed and about to move on"); break;
    }
}

/// Строка о состоянии стадии s: "tunnel: 2/2 posts held by car #15, car #12 (both blocked)".
static void StageText(WgSnap *g, int s, char *buf, int size) {
    const WgStage *w = &g->st[s];
    const StageSpec *spec = &g->cfg->stage[s];
    if (w->posts == 0) {
        snprintf(buf, size, "%s: 0 posts", spec->name);
        return;
    }
    if (w->broken == w->posts) {
        snprintf(buf, size, "%s: %d/%d posts broken", spec->name, w->broken, w->posts);
        return;
    }
    int n = snprintf(buf, size, "%s: %d/%d %s", spec->name, w->holders, w->seats,
                     spec->capacity == 1 ? "posts" : "seats");
    int blocked = 0;
    for (int i = 0; i < w->holders && n < size; ++i) {
        const WgCar *c = FindCar(g, w->holder[i]);
        blocked += c && c->kind == WG_ROOM;
        n += snprintf(buf + n, size - n, "%s car #%d", i ? "," : " held by", w->holder[i] + 1);
    }
    if (w->holders > 0 && n < size) {
        if (blocked == w->holders) {
            n += snprintf(buf + n, size - n, " (%s)",
                          w->holders == 1   ? "blocked"
                          : w->holders == 2 ? "both blocked"
                                            : "all blocked");
        } else if (blocked > 0) {
            n += snprintf(buf + n, size - n, " (%d of %d blocked)", blocked, w->holders);
        }
    }
    if (w->broken > 0 && n < size) {
        n += snprintf(buf + n, size - n, ", %d broken", w->broken);
    }
    if (w->queueCap > 0 && n < size) {
        snprintf(buf + n, size - n, ", queue %d/%d", w->queued, w->queueCap);
    }
}

void WaitGraphPrint(int fd, Line *l, int color) {
    static _Thread_local WgSnap g;  // крупный: не на стеке потока часов
    Collect(l, &g);
    const Config *cfg = l->cfg;
    char clock[16], place[TEXT_LEN], wait[TEXT_LEN], text[TEXT_LEN * 4];
    EventClock(g.now, clock, sizeof clock);
    dprintf(fd, "%sWAITS-FOR GRAPH at %s (%d car(s) stuck)%s\n", color ? BOLD : "", clock, g.stuck,
            color ? RESET : "");
    for (int t = cfg->stages - 1; t >= 0; --t) {
        int group = 0;
        for (int i = 0; i < g.n; ++i) {
            const WgCar *c = &g.car[i];
            if (c->target != t) {
                continue;
            }
            PlaceText(&g, c, place, sizeof place);
            WaitText(&g, c, wait, sizeof wait);
            dprintf(fd, "  car #%-4d%-19s-> %s\n", c->car + 1, place, wait);
            group = 1;
        }
        if (!group) {
            continue;
        }
        StageText(&g, t, text, sizeof text);
        if (t == g.root) {
            int len = (int)strlen(text);
            dprintf(fd, "    %s%*s%s<- ROOT CAUSE: %s%s\n", text,
                    len < STAGE_COLUMN ? STAGE_COLUMN - len : 1, "", color ? RED : "", g.rootText,
                    color ? RESET : "");
        } else {
            dprintf(fd, "    %s\n", text);
        }
    }
    for (int i = 0; i < g.n; ++i) {  // ждут ремонта своего поста
        const WgCar *c = &g.car[i];
        if (c->kind == WG_REPAIR) {
            PlaceText(&g, c, place, sizeof place);
            WaitText(&g, c, wait, sizeof wait);
            dprintf(fd, "  car #%-4d%-19s-> %s\n", c->car + 1, place, wait);
        }
    }
    if (g.stuck == 0) {
        dprintf(fd, "  (nobody is waiting)\n");
    }
}

int WaitGraphDot(Line *l, const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        return -1;
    }
    static _Thread_local WgSnap g;
    Collect(l, &g);
    const Config *cfg = l->cfg;
    char clock[16], place[TEXT_LEN], wait[TEXT_LEN], text[TEXT_LEN * 4];
    EventClock(g.now, clock, sizeof clock);
    dprintf(fd, "digraph waits {\n");
    dprintf(fd, "  label=\"WAITS-FOR GRAPH at %s (%d car(s) stuck)\";\n", clock, g.stuck);
    dprintf(fd, "  rankdir=LR;\n  node [fontname=\"Helvetica\", fontsize=10];\n");
    int wanted[MAX_STAGES] = {0};
    for (int i = 0; i < g.n; ++i) {
        const WgCar *c = &g.car[i];
        PlaceText(&g, c, place, sizeof place);
        WaitText(&g, c, wait, sizeof wait);
        dprintf(fd, "  \"car #%d\" [shape=ellipse, label=\"car #%d\\n%s\\n%s\"];\n", c->car + 1,
                c->car + 1, place, wait);
        if (c->target >= 0) {
            wanted[c->target] = 1;
            dprintf(fd, "  \"car #%d\" -> \"stage %s\" [label=\"%s\"];\n", c->car + 1,
                    cfg->stage[c->target].name, c->kind == WG_ROOM ? "room" : "post");
        }
    }
    for (int s = 0; s < cfg->stages; ++s) {
        if (!wanted[s]) {
            continue;
        }
        StageText(&g, s, text, sizeof text);
        if (s == g.root) {
            dprintf(fd,
                    "  \"stage %s\" [shape=box, color=red, fontcolor=red, penwidth=2, "
                    "label=\"%s\\nROOT CAUSE: %s\"];\n",
                    cfg->stage[s].name, text, g.rootText);
        } else {
            dprintf(fd, "  \"stage %s\" [shape=box, label=\"%s\"];\n", cfg->stage[s].name, text);
        }
        for (int i = 0; i < g.st[s].holders; ++i) {
            dprintf(fd, "  \"stage %s\" -> \"car #%d\" [label=\"held by\", style=dashed];\n",
                    cfg->stage[s].name, g.st[s].holder[i] + 1);
        }
    }
    dprintf(fd, "}\n");
    close(fd);
    return 0;
}

// ---- таблица потоков ----

/// Копия того, что часы знают о потоке.
typedef struct {
    char name[24];
    ThreadKind kind;
    SimState state;
    WaitReason why;
    int wakeAt;
    long parks;
} ThRow;

typedef struct {
    ThRow *row;
    int n, cap;
} ThTable;

/// Под мьютексом часов: только копирование, без Sim* и без других мьютексов.
static void CopyThread(const SimThread *t, void *ctx) {
    ThTable *tab = ctx;
    if (tab->row && tab->n < tab->cap) {
        ThRow *r = &tab->row[tab->n];
        memcpy(r->name, t->name, sizeof r->name);
        r->kind = t->kind;
        r->state = t->state;
        r->why = t->why;
        r->wakeAt = t->wakeAt;
        r->parks = t->parks;
    }
    tab->n++;
}

static const char *StateName(SimState s) {
    switch (s) {
    case ST_READY: return "ready";
    case ST_RUNNING: return "running";
    case ST_SLEEPING: return "sleeping";
    case ST_PARKED: return "parked";
    case ST_DONE:
    default: return "done";
    }
}

static void ReasonText(const ThRow *r, const Config *cfg, char *buf, int size) {
    const char *st =
        r->why.stage >= 0 && r->why.stage < cfg->stages ? cfg->stage[r->why.stage].name : "?";
    if (r->state == ST_SLEEPING) {
        char clock[16];
        EventClock(r->wakeAt, clock, sizeof clock);
        snprintf(buf, size, "time %s", clock);
        return;
    }
    if (r->state != ST_PARKED) {
        snprintf(buf, size, "-");
        return;
    }
    switch (r->why.kind) {
    case WAIT_QUEUE: snprintf(buf, size, "a post in %s (in queue)", st); break;
    case WAIT_OPERATION:
        snprintf(buf, size, "end of operation on %s post %d", st, r->why.post + 1);
        break;
    case WAIT_ROOM: snprintf(buf, size, "room in %s", st); break;
    case WAIT_REPAIR: snprintf(buf, size, "repair of %s post %d", st, r->why.post + 1); break;
    case WAIT_TIME: snprintf(buf, size, "time"); break;
    case WAIT_NONE:
    default: snprintf(buf, size, "a wake-up"); break;
    }
}

void WaitGraphThreads(int fd, const Config *cfg) {
    ThTable tab = {0};
    SimForEach(CopyThread, &tab);  // сколько потоков (в покое их число не меняется)
    tab.cap = tab.n + 8;
    tab.row = calloc((size_t)tab.cap, sizeof *tab.row);
    tab.n = 0;
    if (tab.row) {
        SimForEach(CopyThread, &tab);
    }
    int n = tab.n < tab.cap ? tab.n : tab.cap;
    int kinds[3] = {0}, states[5] = {0};
    for (int i = 0; tab.row && i < n; ++i) {
        kinds[tab.row[i].kind]++;
        states[tab.row[i].state]++;
    }
    char clock[16];
    EventClock(SimNow(), clock, sizeof clock);
    dprintf(fd,
            "THREADS at %s: %d model thread(s) (coordinator %d, posts %d, cars %d); running %d, "
            "ready %d, sleeping %d, parked %d, done %d\n",
            clock, tab.n, kinds[TH_COORD], kinds[TH_POST], kinds[TH_CAR], states[ST_RUNNING],
            states[ST_READY], states[ST_SLEEPING], states[ST_PARKED], states[ST_DONE]);
    if (!tab.row) {
        return;
    }
    dprintf(fd, "  %-16s %-9s %6s  %s\n", "thread", "state", "parks", "waits for");
    for (int i = 0; i < n; ++i) {
        char why[TEXT_LEN];
        ReasonText(&tab.row[i], cfg, why, sizeof why);
        dprintf(fd, "  %-16s %-9s %6ld  %s\n", tab.row[i].name, StateName(tab.row[i].state),
                tab.row[i].parks, why);
    }
    free(tab.row);
}
