/*
 * Сервер автомойки. Единственный владелец состояния линии (State): принимает TCP-соединения
 * постов, источника машин и наблюдателей, читает их сообщения через poll и меняет состояние
 * функциями St* в одном потоке, поэтому блокировки не нужны.
 *
 * Машины - не отдельные клиенты, а записи в State: куда машине ехать дальше, решает сервер.
 * Посты - клиенты, которые выполняют операции (сами выбирают длительность, ломаются и
 * чинятся). Источник машин - клиент, который решает, когда приезжает машина и какую
 * программу она выбирает. Наблюдатели получают ленту событий и снимки состояния.
 *
 * Любой клиент может отключиться и подключиться снова, сервер продолжает работу: пост,
 * пропавший посреди операции, после возвращения выполняет её заново; без источника машин
 * просто нет новых прибытий. При остановке сервер рассылает всем STOP и ждёт, пока клиенты
 * закроют соединения.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net.h"
#include "soft_assert.h"
#include "state.h"
#include "util.h"

#define MAX_CLIENTS      64
#define LEAVE_TIMEOUT_MS 3000  ///< сколько ждать отключения клиентов после STOP
#define SNAP_EVERY_MS    100   ///< не чаще - снимки наблюдателям
#define SNAP_IDLE_MS     500   ///< не реже - чтобы часы у наблюдателей шли
#define RECENT_EVENTS    14    ///< сколько последних событий получает новый наблюдатель

typedef enum { ROLE_NEW, ROLE_POST, ROLE_CARS, ROLE_VIEW } Role;

/// Подключённый клиент.
typedef struct {
    LineConn io;
    int role;         ///< Role
    int stage, post;  ///< для поста
    int pid;          ///< процесс клиента на его машине (присылает в HELLO)
    char addr[48];    ///< "ip:порт" клиента
} Client;

static State st;
static Client cl[MAX_CLIENTS];
static int listenFd = -1;
static const char *bindIp;
static int bindPort;

static int postClient[MAX_STAGES][MAX_POSTS];  ///< клиент поста или -1 (пост не в сети)
static int opRunning[MAX_STAGES][MAX_POSTS];   ///< пост выполняет операцию над своей машиной
/// Пост сказал, что ждёт машину (подключился свободным или прислал IDLE). Только такой пост
/// получает SERVE: после REPAIR пост свободен, но его IDLE ещё в пути.
static int postReady[MAX_STAGES][MAX_POSTS];
static int sourceClient = -1;
static int postsOnline = 0;

/// Машина закончила операцию, но в накопителе следующей стадии нет места: ждёт на посту.
static int blockedFor[MAX_SLOTS];  ///< стадия, место в которой ждёт машина, или -1
static long blockedSeq[MAX_SLOTS];
static long blockSeq = 0;

static StopReason stopping = STOP_NONE;
static int snapDirty = 1;
static long long lastSnapNs = 0;

/// Последние события - для наблюдателя, который подключился посреди дня.
static char recent[RECENT_EVENTS][LINE_MAX_LEN];
static char recentColor[RECENT_EVENTS];
static int recentCount = 0, recentHead = 0;

static const char *const stateWord[] = {"off", "idle", "busy", "broken"};

/* ---------------------------------------------------------------- клиенты */

static int ClientCount(int role) {
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        n += cl[i].io.fd >= 0 && cl[i].role == role;
    }
    return n;
}

static void Send(int c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void DropClient(int c, const char *why);

/// Отправляет строку клиенту; не получилось - клиент отключается.
static void Send(int c, const char *fmt, ...) {
    if (c < 0 || cl[c].io.fd < 0) {
        return;
    }
    char line[LINE_MAX_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (NetSend(cl[c].io.fd, "%s", line) != 0) {
        DropClient(c, "connection lost");
    }
}

/// Посту: машина @p carId съехала, пост свободен.
static void SendFree(int stage, int post, int carId) {
    if (!SOFT_ASSERT(stage >= 0 && stage < st.cfg.stages && post >= 0 &&
                         post < st.cfg.stage[stage].posts,
                     "FREE for a post that does not exist")) {
        return;
    }
    if (postClient[stage][post] >= 0) {
        Send(postClient[stage][post], "FREE %d", carId);
    }
}

/// Буква цвета события для наблюдателей: какая сущность и что с ней.
static char ColorLetter(const char *color) {
    static const struct {
        const char *c;
        char l;
    } map[] = {{C_CAR, 'c'},  {C_WAIT, 'w'},  {C_FAIL, 'f'}, {C_FIX, 'x'},
               {C_DONE, 'd'}, {C_COORD, 'k'}, {C_POST, 'p'}};
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); ++i) {
        if (strcmp(color, map[i].c) == 0) {
            return map[i].l;
        }
    }
    return 'n';
}

/// Каждое событие ленты сервера уходит и всем наблюдателям.
static void OnOutput(const char *line, const char *color) {
    char l = ColorLetter(color);
    snprintf(recent[recentHead], LINE_MAX_LEN, "%s", line);
    recentColor[recentHead] = l;
    recentHead = (recentHead + 1) % RECENT_EVENTS;
    if (recentCount < RECENT_EVENTS) {
        ++recentCount;
    }
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (cl[i].io.fd >= 0 && cl[i].role == ROLE_VIEW) {
            Send(i, "EV %c %s", l, line);
        }
    }
    snapDirty = 1;
}

/* ---------------------------------------------------------------- снимок для наблюдателей */

/// Сколько машин в накопителе стадии, номера - по порядку постановки в очередь.
static int QueueIds(int stage, char *buf, int size) {
    int slots[MAX_SLOTS], n = 0;
    for (int i = 0; i < MAX_SLOTS; ++i) {
        if (st.car[i].state == CAR_QUEUE && st.car[i].stage == stage) {
            slots[n++] = i;
        }
    }
    for (int i = 1; i < n; ++i) {  // вставками: машин в очереди меньше MAX_QUEUE
        for (int k = i; k > 0 && st.car[slots[k]].seq < st.car[slots[k - 1]].seq; --k) {
            int t = slots[k];
            slots[k] = slots[k - 1];
            slots[k - 1] = t;
        }
    }
    int len = 0;
    buf[0] = '\0';
    for (int i = 0; i < n && len < size; ++i) {
        len += snprintf(buf + len, size - len, " %d:%d", st.car[slots[i]].id,
                        st.car[slots[i]].program);
    }
    return n;
}

/// Снимок состояния одной пачкой строк: SNAP, Q на стадию, P на пост, END.
static void SendSnapshot(int c) {
    char out[NET_BUF];
    int n =
        snprintf(out, sizeof(out), "SNAP %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
                 st.startNs ? StNow(&st) : 0, st.startNs != 0, st.closed, st.active, st.st.arrived,
                 st.st.accepted, st.st.finished, st.st.turned, st.st.refused, sourceClient >= 0,
                 ClientCount(ROLE_VIEW), stopping != STOP_NONE, st.violations);
    for (int s = 0; s < st.cfg.stages && n < (int)sizeof(out); ++s) {
        char ids[400];
        int len = QueueIds(s, ids, sizeof(ids));
        n += snprintf(out + n, sizeof(out) - n, "Q %d %d%s\n", s, len, ids);
    }
    for (int s = 0; s < st.cfg.stages; ++s) {
        for (int p = 0; p < st.cfg.stage[s].posts && n < (int)sizeof(out); ++p) {
            const PostSlot *ps = &st.post[s][p];
            int id = 0, program = -1, wait = -1;
            if (ps->car >= 0) {
                const CarSlot *car = &st.car[ps->car];
                id = car->id;
                program = car->program;
                // машина отработала здесь и ждёт следующую стадию
                wait = blockedFor[ps->car] >= 0 ? blockedFor[ps->car] : car->waitStage;
            }
            n += snprintf(out + n, sizeof(out) - n, "P %d %d %s %d %d %d %d %d %d %d\n", s, p,
                          stateWord[ps->state], postClient[s][p] >= 0, id, program, opRunning[s][p],
                          wait, ps->ops, ps->fails);
        }
    }
    if (n < (int)sizeof(out) - 5) {
        n += snprintf(out + n, sizeof(out) - n, "END\n");
        if (WriteAll(cl[c].io.fd, out, (size_t)n) != 0) {
            DropClient(c, "connection lost");
        }
    }
}

static void BroadcastSnapshot(void) {
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (cl[i].io.fd >= 0 && cl[i].role == ROLE_VIEW) {
            SendSnapshot(i);
        }
    }
    snapDirty = 0;
    lastSnapNs = NowNs();
}

/* ---------------------------------------------------------------- движение машин */

static void Dispatch(int stage);

/// Машина уходит к следующей стадии маршрута (место в накопителе уже есть).
/// Dispatch -> MoveOn -> Unblock -> MoveOn: цепочка рекурсивна, но каждый шаг двигает машину
/// по маршруту дальше, так что глубина ограничена длиной маршрута и числом машин.
static void MoveOn(int slot) {  // NOLINT(misc-no-recursion)
    CarSlot *c = &st.car[slot];
    int from = c->stage;
    int freed = StMoveOn(&st, slot);
    if (freed >= 0) {
        SendFree(from, freed, c->id);
    }
    Dispatch(st.cfg.program[c->program].route[c->step]);
}

/// В накопителе стадии освободилось место: первой туда едет машина, дольше всех ждущая его.
static void Unblock(int stage) {  // NOLINT(misc-no-recursion)
    int best = -1;
    for (int i = 0; i < MAX_SLOTS; ++i) {
        if (st.car[i].state != CAR_FREE && blockedFor[i] == stage &&
            (best < 0 || blockedSeq[i] < blockedSeq[best])) {
            best = i;
        }
    }
    if (best >= 0) {
        blockedFor[best] = -1;
        MoveOn(best);
    }
}

/// Свободные посты стадии, клиенты которых в сети, забирают ожидающие машины.
static void Dispatch(int stage) {  // NOLINT(misc-no-recursion)
    if (stopping) {
        return;
    }
    for (int p = 0; p < st.cfg.stage[stage].posts; ++p) {
        PostSlot *ps = &st.post[stage][p];
        if (ps->state != POST_IDLE || ps->car >= 0 || postClient[stage][p] < 0 ||
            !postReady[stage][p] || !StHasWaiting(&st, stage)) {
            continue;
        }
        int queueFreed, prevStage, prevPost;
        int car = StDispatch(&st, stage, p, &queueFreed, &prevStage, &prevPost);
        if (prevPost >= 0) {
            SendFree(prevStage, prevPost, st.car[car].id);
        }
        postReady[stage][p] = 0;
        Send(postClient[stage][p], "SERVE %d %s", st.car[car].id,
             st.cfg.program[st.car[car].program].name);
        if (queueFreed) {
            Unblock(stage);
        }
    }
}

/// Пост закончил операцию над машиной: дальше по маршруту или домой.
static void CarDone(int slot) {
    CarSlot *c = &st.car[slot];
    const ProgramSpec *prog = &st.cfg.program[c->program];
    if (c->step + 1 == prog->len) {
        int id = c->id, program = c->program, stage = c->stage;
        int total = StNow(&st) - c->arrived;
        int post = StFinish(&st, slot);
        SendFree(stage, post, id);
        Send(sourceClient, "WASHED %d %s %d", id, st.cfg.program[program].name, total);
        return;
    }
    int next = prog->route[c->step + 1];
    const StageSpec *ns = &st.cfg.stage[next];
    if (ns->queue > 0 && st.queueLen[next] >= ns->queue) {
        StBlocked(&st, slot);
        blockedFor[slot] = next;
        blockedSeq[slot] = ++blockSeq;
        return;
    }
    MoveOn(slot);
}

/* ---------------------------------------------------------------- остановка и въезд */

static void RequestStop(StopReason why) {
    if (stopping != STOP_NONE) {
        return;
    }
    stopping = why;
    st.endMinute = st.startNs ? StNow(&st) : 0;
    static const char *text[] = {"", "the day is over", "no program can be served",
                                 "the server was interrupted", "server failure"};
    OutEvent(st.endMinute, "server", getpid(), C_COORD, "stopping: %s, disconnecting clients",
             text[why]);
    StReport(&st, why);  // до STOP, чтобы итоги успели получить и наблюдатели
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (cl[i].io.fd >= 0) {
            Send(i, "STOP %d %s", StExitCode(&st, why), text[why]);
        }
    }
}

static void CheckDone(void) {
    if (!stopping && st.startNs && st.closed && st.active == 0) {
        RequestStop(STOP_DONE);
    }
}

static void CloseEntrance(const char *why) {
    if (st.closed) {
        return;
    }
    StClose(&st, why);
    Send(sourceClient, "CLOSED");
    CheckDone();
}

static void OpenEntranceIfReady(void) {
    if (st.startNs == 0 && !stopping && postsOnline == ConfigTotalPosts(&st.cfg) &&
        sourceClient >= 0) {
        st.startNs = NowNs();
        OutEvent(0, "server", getpid(), C_COORD,
                 "all %d posts and the car source are online, entrance opened", postsOnline);
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (cl[i].io.fd >= 0 && cl[i].role != ROLE_NEW) {
                Send(i, "OPEN 0");
            }
        }
    }
}

/* ---------------------------------------------------------------- регистрация */

static void Reject(int c, const char *why) {
    NetSend(cl[c].io.fd, "REJECT %s", why);
    dprintf(1, "client %s rejected: %s\n", cl[c].addr, why);
    LineClose(&cl[c].io);
}

static void HelloPost(int c, const char *stageName, int number) {
    int s = -1;
    for (int i = 0; i < st.cfg.stages; ++i) {
        if (strcmp(st.cfg.stage[i].name, stageName) == 0) {
            s = i;
        }
    }
    char why[96];
    if (s < 0) {
        snprintf(why, sizeof(why), "no stage '%s' in the line", stageName);
        Reject(c, why);
        return;
    }
    int p = number - 1;
    if (p < 0 || p >= st.cfg.stage[s].posts) {
        snprintf(why, sizeof(why), "stage %s has posts 1..%d", stageName, st.cfg.stage[s].posts);
        Reject(c, why);
        return;
    }
    if (postClient[s][p] >= 0) {
        snprintf(why, sizeof(why), "post %s.%d is already online", stageName, number);
        Reject(c, why);
        return;
    }
    const StageSpec *spec = &st.cfg.stage[s];
    cl[c].role = ROLE_POST;
    cl[c].stage = s;
    cl[c].post = p;
    postClient[s][p] = c;
    ++postsOnline;
    PostSlot *ps = &st.post[s][p];
    int wasOpen = st.startNs != 0;
    int wasBroken = ps->state == POST_BROKEN;
    ps->pid = cl[c].pid;
    ps->state = ps->car >= 0 ? POST_BUSY : POST_IDLE;
    postReady[s][p] = ps->car < 0;
    Send(c, "WELCOME post %s %d %d %d %.4f %d %d %u %d %d %d %d", spec->name, number, spec->tmin,
         spec->tmax, st.cfg.failP, st.cfg.repairMin, st.cfg.repairMax, st.cfg.seed, st.cfg.delayMs,
         st.startNs ? StNow(&st) : 0, st.startNs != 0, s);

    char who[24];
    StPostName(&st, s, p, who, sizeof(who));
    if (ps->car >= 0 && opRunning[s][p]) {
        OutEvent(StNow(&st), who, ps->pid, C_FIX,
                 "back online from %s, restarts the operation on car #%d", cl[c].addr,
                 st.car[ps->car].id);
        Send(c, "SERVE %d %s", st.car[ps->car].id, st.cfg.program[st.car[ps->car].program].name);
    } else if (ps->car >= 0) {
        OutEvent(StNow(&st), who, ps->pid, C_FIX, "back online from %s, car #%d is still on it",
                 cl[c].addr, st.car[ps->car].id);
    } else if (st.startNs) {
        OutEvent(StNow(&st), who, ps->pid, C_FIX, "back online from %s%s", cl[c].addr,
                 wasBroken ? ", repaired while offline" : "");
    } else {
        dprintf(1, "post %s connected from %s (pid %d), posts online %d/%d\n", who, cl[c].addr,
                cl[c].pid, postsOnline, ConfigTotalPosts(&st.cfg));
    }
    if (wasOpen) {
        Send(c, "OPEN %d", StNow(&st));
    } else {
        OpenEntranceIfReady();
    }
    Dispatch(s);
}

static void HelloCars(int c) {
    if (sourceClient >= 0) {
        Reject(c, "a car source is already online");
        return;
    }
    cl[c].role = ROLE_CARS;
    sourceClient = c;
    int remaining = st.cfg.cars > 0 ? st.cfg.cars - st.st.arrived : -1;
    char line[LINE_MAX_LEN];
    int n = snprintf(line, sizeof(line), "WELCOME cars %d %d %d %u %d %d %d %d", st.cfg.arrMin,
                     st.cfg.arrMax, remaining, st.cfg.seed, st.cfg.delayMs,
                     st.startNs ? StNow(&st) : 0, st.nextId, st.cfg.programs);
    for (int i = 0; i < st.cfg.programs && n < (int)sizeof(line); ++i) {
        n += snprintf(line + n, sizeof(line) - n, " %s %d", st.cfg.program[i].name,
                      st.cfg.program[i].weight);
    }
    Send(c, "%s", line);
    if (st.startNs) {
        OutEvent(StNow(&st), "entrance", cl[c].pid, C_FIX, "car source is back online from %s",
                 cl[c].addr);
        Send(c, "OPEN %d", StNow(&st));
        if (st.closed) {
            Send(c, "CLOSED");
        }
    } else {
        dprintf(1, "car source connected from %s (pid %d)\n", cl[c].addr, cl[c].pid);
        OpenEntranceIfReady();
    }
}

static void HelloView(int c) {
    cl[c].role = ROLE_VIEW;
    const Config *cfg = &st.cfg;
    Send(c, "WELCOME view %d %d %d %d %d %s %s:%d", cfg->stages, cfg->programs, cfg->day, cfg->cars,
         cfg->delayMs, ConfigRuleName(cfg->rule), bindIp, bindPort);
    for (int s = 0; s < cfg->stages; ++s) {
        const StageSpec *sp = &cfg->stage[s];
        Send(c, "STAGE %d %s %d %d %d %d", s, sp->name, sp->posts, sp->queue, sp->tmin, sp->tmax);
    }
    for (int i = 0; i < cfg->programs; ++i) {
        const ProgramSpec *p = &cfg->program[i];
        char route[64] = "";
        int n = 0;
        for (int k = 0; k < p->len; ++k) {
            n += snprintf(route + n, sizeof(route) - n, " %d", p->route[k]);
        }
        Send(c, "PROGRAM %d %s %d%s", i, p->name, p->len, route);
    }
    for (int k = 0; k < recentCount; ++k) {
        int i = (recentHead - recentCount + k + RECENT_EVENTS) % RECENT_EVENTS;
        Send(c, "EV %c %s", recentColor[i], recent[i]);
    }
    dprintf(1, "viewer connected from %s, viewers online %d\n", cl[c].addr, ClientCount(ROLE_VIEW));
    snapDirty = 1;
    if (cl[c].io.fd >= 0) {
        SendSnapshot(c);
    }
}

static void OnHello(int c, const char *line) {
    char role[16] = "", stage[NAME_LEN + 4] = "";
    int pid = 0, number = 0;
    int got = sscanf(line, "HELLO %15s %d %15s %d", role, &pid, stage, &number);
    cl[c].pid = pid;
    if (stopping) {
        Reject(c, "the server is stopping");
    } else if (got >= 2 && strcmp(role, "view") == 0) {
        HelloView(c);
    } else if (got >= 2 && strcmp(role, "cars") == 0) {
        HelloCars(c);
    } else if (got == 4 && strcmp(role, "post") == 0) {
        HelloPost(c, stage, number);
    } else {
        Reject(c, "expected: HELLO post PID STAGE N | HELLO cars PID | HELLO view PID");
    }
}

/// Клиент отключился (BYE, конец соединения, ошибка записи).
static void DropClient(int c, const char *why) {
    if (cl[c].io.fd < 0) {
        return;
    }
    Client *k = &cl[c];
    LineClose(&k->io);
    int now = st.startNs ? StNow(&st) : 0;
    if (k->role == ROLE_POST) {
        int s = k->stage, p = k->post;
        postClient[s][p] = -1;
        --postsOnline;
        postReady[s][p] = 0;
        PostSlot *ps = &st.post[s][p];
        ps->state = POST_OFF;
        char who[24], extra[64] = "";
        StPostName(&st, s, p, who, sizeof(who));
        if (ps->car >= 0) {
            snprintf(extra, sizeof(extra), "; car #%d waits on it%s", st.car[ps->car].id,
                     opRunning[s][p] ? ", the operation will be redone" : "");
        }
        if (!stopping) {
            OutEvent(now, who, k->pid, C_FAIL, "client %s: post is offline%s", why, extra);
        }
    } else if (k->role == ROLE_CARS) {
        sourceClient = -1;
        if (!stopping) {
            OutEvent(now, "entrance", k->pid, C_FAIL, "car source %s: no new cars until it is back",
                     why);
        }
    } else if (k->role == ROLE_VIEW && !stopping) {
        dprintf(1, "viewer %s left (%s), viewers online %d\n", k->addr, why,
                ClientCount(ROLE_VIEW));
    }
    k->role = ROLE_NEW;
    snapDirty = 1;
}

/* ---------------------------------------------------------------- сообщения клиентов */

static void OnPost(int c, const char *line) {
    int s = cl[c].stage, p = cl[c].post, m = 0;
    PostSlot *ps = &st.post[s][p];
    if (sscanf(line, "START %d", &m) == 1) {
        opRunning[s][p] = 1;
        StOpStart(&st, s, p, m);
    } else if (sscanf(line, "END %d", &m) == 1) {
        int car = ps->car;
        opRunning[s][p] = 0;
        StOpEnd(&st, s, p, m);
        if (car >= 0) {
            CarDone(car);
        }
    } else if (sscanf(line, "BREAK %d", &m) == 1) {
        StBreak(&st, s, p, m);
    } else if (strcmp(line, "REPAIR") == 0) {
        StRepair(&st, s, p);
    } else if (strcmp(line, "IDLE") == 0) {
        StPostFree(&st, s, p);
        postReady[s][p] = 1;
        Dispatch(s);
    }
}

static void OnCars(int c, const char *line) {
    int program, n;
    if (sscanf(line, "ARRIVE %d", &program) == 1) {
        if (!st.startNs) {
            return;
        }
        if (st.cfg.day > 0 && StNow(&st) >= st.cfg.day) {
            CloseEntrance("end of the working day");
        }
        int slot = st.closed ? -1 : StReserveCar(&st);
        if (slot < 0) {
            Send(c, "CLOSED");
            return;
        }
        if (program < 0 || program >= st.cfg.programs) {
            program = 0;
        }
        const ProgramSpec *prog = &st.cfg.program[program];
        int first = prog->route[0];
        int dead = ConfigDeadStage(&st.cfg, program);
        int room = st.cfg.stage[first].queue > 0 && st.queueLen[first] < st.cfg.stage[first].queue;
        int id = st.car[slot].id;
        if (StArrive(&st, slot, program, cl[c].pid, dead < 0 && room)) {
            blockedFor[slot] = -1;
            Send(c, "CAR %d %s accepted", id, prog->name);
            Dispatch(first);
        } else {
            Send(c, "CAR %d %s %s", id, prog->name, dead >= 0 ? "refused" : "turned");
        }
    } else if (sscanf(line, "CLOSE %d", &n) == 1) {
        char why[48];
        snprintf(why, sizeof(why), "all %d cars have arrived", n);
        CloseEntrance(why);
    }
}

static void Handle(int c, const char *line) {
    if (strncmp(line, "HELLO ", 6) == 0 && cl[c].role == ROLE_NEW) {
        OnHello(c, line);
        return;
    }
    if (strcmp(line, "BYE") == 0) {
        DropClient(c, "disconnected");
        return;
    }
    if (stopping) {
        return;
    }
    if (cl[c].role == ROLE_POST) {
        OnPost(c, line);
    } else if (cl[c].role == ROLE_CARS) {
        OnCars(c, line);
    }
    snapDirty = 1;
    CheckDone();
}

static void Accept(void) {
    int fd = accept(listenFd, NULL, NULL);
    if (fd < 0) {
        return;
    }
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        if (cl[i].io.fd < 0) {
            memset(&cl[i], 0, sizeof(cl[i]));
            LineInit(&cl[i].io, fd);
            cl[i].role = ROLE_NEW;
            NetPeerName(fd, cl[i].addr, sizeof(cl[i].addr));
            if (stopping) {
                Reject(i, "the server is stopping");
            }
            return;
        }
    }
    NetSend(fd, "REJECT too many clients");
    close(fd);
}

static void ReadClient(int c) {
    int r = LineFill(&cl[c].io);
    if (r == -2) {
        return;
    }
    if (r <= 0) {
        DropClient(c, r == 0 ? "closed the connection" : "connection error");
        return;
    }
    char line[LINE_MAX_LEN];
    while (cl[c].io.fd >= 0 && LineNext(&cl[c].io, line, sizeof(line))) {
        Handle(c, line);
    }
}

static void HandleSignals(void) {
    if (gotInterrupt) {
        gotInterrupt = 0;
        RequestStop(STOP_INTERRUPT);
    }
    if (gotStatus) {
        gotStatus = 0;
        StStatus(&st);
        OutLine("  clients: %d/%d posts online, car source %s, %d viewer(s)", postsOnline,
                ConfigTotalPosts(&st.cfg), sourceClient >= 0 ? "online" : "offline",
                ClientCount(ROLE_VIEW));
    }
    if (gotClose) {
        gotClose = 0;
        if (st.startNs) {
            CloseEntrance("closed early by SIGUSR2");
        }
    }
}

/// Проверки по часам модели: конец рабочего дня.
static void Tick(void) {
    if (!stopping && st.startNs && !st.closed && st.cfg.day > 0 && StNow(&st) >= st.cfg.day) {
        CloseEntrance("end of the working day");
    }
    long long sinceSnap = (NowNs() - lastSnapNs) / 1000000LL;
    if ((snapDirty && sinceSnap >= SNAP_EVERY_MS) || sinceSnap >= SNAP_IDLE_MS) {
        BroadcastSnapshot();
    }
}

static int ClientsLeft(void) {
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        n += cl[i].io.fd >= 0;
    }
    return n;
}

static void Usage(int fd) {
    dprintf(fd, "Usage: cw-server [-c FILE] [-s SEED] [-d MS] [-l FILE] [-u] [-q] IP PORT\n"
                "  IP    interface to listen on: 127.0.0.1 - this computer only, 0.0.0.0 - all\n"
                "  PORT  TCP port, e.g. 5050\n"
                "Then start, in any order, on this or other computers:\n"
                "  cw-post SERVER PORT STAGE N   one client per post\n"
                "  cw-cars SERVER PORT           the car source\n"
                "  cw-view SERVER PORT           a viewer, as many as you like\n"
                "or scripts/run.sh to start everything here at once.\n"
                "Signals: SIGINT/SIGTERM stop the line and all clients, SIGUSR1 prints the state,\n"
                "SIGUSR2 closes the entrance early.\n");
}

static int OnExtra(int opt, const char *arg) {
    (void)arg;
    if (opt == 'h') {
        Usage(1);
        exit(0);
    }
    return -1;
}

int main(int argc, char **argv) {
    Config cfg;
    if (ConfigFromArgs(&cfg, argc, argv, "h", OnExtra) != 0) {
        return 1;
    }
    if (argc - optind != 2) {
        Usage(2);
        return 1;
    }
    bindIp = argv[optind];
    if (NetParsePort(argv[optind + 1], &bindPort) != 0) {
        return 1;
    }
    int journal = open(cfg.journal, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (journal < 0) {
        dprintf(2, "cw-server: cannot open journal %s: %s\n", cfg.journal, strerror(errno));
        return 1;
    }
    OutInit(journal, cfg.quiet);
    SignalsInstall();
    if (StateInit(&st, &cfg) != 0) {
        dprintf(2, "cw-server: the line holds too many cars (max %d)\n", MAX_SLOTS);
        return 1;
    }
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        cl[i].io.fd = -1;
    }
    for (int s = 0; s < MAX_STAGES; ++s) {
        for (int p = 0; p < MAX_POSTS; ++p) {
            postClient[s][p] = -1;
        }
    }
    for (int i = 0; i < MAX_SLOTS; ++i) {
        blockedFor[i] = -1;
    }
    char err[200];
    listenFd = NetListen(bindIp, bindPort, err, sizeof(err));
    if (listenFd < 0) {
        dprintf(2, "cw-server: %s\n", err);
        return 5;
    }
    OutSetHook(OnOutput);
    OutLine("Car wash, variant 43: TCP server on %s:%d (pid %d)", bindIp, bindPort, (int)getpid());
    ConfigPrint(&cfg, 1);
    ConfigPrint(&cfg, journal);
    if (ConfigPossiblePrograms(&cfg) == 0) {
        OutEvent(0, "server", getpid(), C_FAIL,
                 "no program can be served: every route needs a stage without posts");
        RequestStop(STOP_IMPOSSIBLE);
    } else {
        dprintf(1, "waiting for %d posts and the car source to connect...\n",
                ConfigTotalPosts(&cfg));
    }

    long long leaveDeadline = 0;
    for (;;) {
        HandleSignals();  // сигнал мог прийти вне poll - тогда EINTR не будет
        Tick();
        if (stopping && leaveDeadline == 0) {
            leaveDeadline = NowNs() + (LEAVE_TIMEOUT_MS * 1000000LL);
        }
        if (stopping && (ClientsLeft() == 0 || NowNs() > leaveDeadline)) {
            break;
        }
        struct pollfd pf[MAX_CLIENTS + 1];
        int who[MAX_CLIENTS + 1], n = 0;
        pf[n].fd = listenFd;
        pf[n].events = POLLIN;
        who[n++] = -1;
        for (int i = 0; i < MAX_CLIENTS; ++i) {
            if (cl[i].io.fd >= 0) {
                pf[n].fd = cl[i].io.fd;
                pf[n].events = POLLIN;
                who[n++] = i;
            }
        }
        int r = poll(pf, (nfds_t)n, SNAP_EVERY_MS);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            dprintf(2, "cw-server: poll: %s\n", strerror(errno));
            RequestStop(STOP_FAILURE);
            continue;
        }
        for (int k = 0; k < n && r > 0; ++k) {
            if (!pf[k].revents) {
                continue;
            }
            if (who[k] < 0) {
                Accept();
            } else if (cl[who[k]].io.fd == pf[k].fd) {
                ReadClient(who[k]);
            }
        }
    }

    int code = StExitCode(&st, stopping);
    for (int i = 0; i < MAX_CLIENTS; ++i) {
        LineClose(&cl[i].io);
    }
    close(listenFd);
    close(journal);
    return code;
}
