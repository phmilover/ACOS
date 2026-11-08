/*
 * Программа 4, координатор. Независимые процессы cw4-coord, cw4-post и cw4-cars общаются
 * только через именованные каналы и именованные семафоры (см. proto.h).
 *
 * Координатор - единственный владелец состояния линии. Он читает сообщения из общего канала
 * по одному, меняет State теми же функциями St*, что и программы 1-3, печатает события и
 * рассылает ответы: посту - «прими машину», машине - «операция закончена». Поэтому мьютекс
 * не нужен: все изменения состояния делает один процесс последовательно.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#include "proto.h"
#include "state.h"
#include "util.h"

#define LEAVE_TIMEOUT_MS 5000

/// Канал на запись к каждому участнику.
typedef struct {
    int fd;  ///< -1 - участника нет
    int pid;
} Peer;

static const char *key = "cw4";
static State st;
static int coordFd = -1;
static char coordPath[64], dirPath[48];
static sem_t *queueSem[MAX_STAGES];
static sem_t *openSem = SEM_FAILED;

static Peer postPeer[MAX_STAGES][MAX_POSTS];
static Peer carPeer[MAX_SLOTS];
static Peer sourcePeer;
static int postsHere = 0;
static StopReason stopping = STOP_NONE;

static int OnExtra(int opt, const char *arg) {
    if (opt == 'k') {
        key = arg;
        // имя семафора "/<key>.open" на macOS не длиннее 31 символа
        return strlen(arg) <= 20 ? 0 : -1;
    }
    if (opt == 'h') {
        dprintf(1, "Usage: cw4-coord [-k KEY] [-c FILE] [-s SEED] [-d MS] [-l FILE] [-u] [-q]\n"
                   "Then start, in any order and from any terminal:\n"
                   "  cw4-post [-k KEY] STAGE NUMBER   one process per post\n"
                   "  cw4-cars [-k KEY]                the car source\n"
                   "or run scripts/run4.sh to start everything at once.\n"
                   "Signals to any participant: SIGINT/SIGTERM stop the whole line;\n"
                   "to the coordinator: SIGUSR1 prints the line state, SIGUSR2 closes the "
                   "entrance.\n");
        exit(0);
    }
    return -1;
}

static void PeerReset(Peer *p) {
    p->fd = -1;
    p->pid = 0;
}

static void PeerDrop(Peer *p) {
    if (p->fd >= 0) {
        close(p->fd);
    }
    PeerReset(p);
}

/// Отправляет участнику сообщение; участник пропал - закрываем его канал.
static void Send(Peer *p, int type, int a, int b) {
    if (p->fd < 0) {
        return;
    }
    Msg m = {type, -1, (int)getpid(), a, b, ""};
    if (WriteAll(p->fd, &m, sizeof(m)) != 0) {
        PeerDrop(p);
    }
}

/// Открывает канал участника на запись и отвечает на регистрацию.
static int Answer(Peer *p, int pid, int accept) {
    char path[64];
    snprintf(path, sizeof(path), "%s/p%d", dirPath, pid);
    int fd = open(path, O_WRONLY | O_NONBLOCK);  // читатель уже открыл канал - не блокируемся
    if (fd < 0) {
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    Welcome w;
    memset(&w, 0, sizeof(w));
    w.head.type = accept ? M_WELCOME : M_STOP;
    w.head.pid = getpid();
    w.cfg = st.cfg;
    if (WriteAll(fd, &w, sizeof(w)) != 0 || !accept) {
        close(fd);
        return accept ? -1 : 0;
    }
    p->fd = fd;
    p->pid = pid;
    return 0;
}

/// Свободные посты стадии забирают ожидающие машины.
static void Dispatch(int stage) {
    if (stopping) {
        return;
    }
    for (int p = 0; p < st.cfg.stage[stage].posts; ++p) {
        PostSlot *ps = &st.post[stage][p];
        if (ps->state != POST_IDLE || ps->car >= 0 || postPeer[stage][p].fd < 0 ||
            !StHasWaiting(&st, stage)) {
            continue;
        }
        int queueFreed, prevStage, prevPost;
        int car = StDispatch(&st, stage, p, &queueFreed, &prevStage, &prevPost);
        if (queueFreed) {
            sem_post(queueSem[stage]);
        }
        if (prevPost >= 0) {
            Send(&postPeer[prevStage][prevPost], M_FREE, 0, 0);
        }
        Send(&postPeer[stage][p], M_SERVE, car, st.car[car].id);
    }
}

/// Ячейка машины по pid её процесса, или -1.
static int CarByPid(int pid) {
    for (int i = 0; i < MAX_SLOTS; ++i) {
        if (st.car[i].state != CAR_FREE && carPeer[i].pid == pid) {
            return i;
        }
    }
    return -1;
}

/// Пост по pid его процесса. @return 0 и номер в *s, *p или -1
static int PostByPid(int pid, int *s, int *p) {
    for (int i = 0; i < st.cfg.stages; ++i) {
        for (int k = 0; k < st.cfg.stage[i].posts; ++k) {
            if (postPeer[i][k].pid == pid) {
                *s = i;
                *p = k;
                return 0;
            }
        }
    }
    return -1;
}

static void RequestStop(StopReason why) {
    if (stopping != STOP_NONE) {
        return;
    }
    stopping = why;
    st.endMinute = st.startNs ? StNow(&st) : 0;
    if (why == STOP_INTERRUPT) {
        OutEvent(st.endMinute, "coordinator", getpid(), C_COORD,
                 "interrupted: stopping all processes");
    }
    for (int s = 0; s < st.cfg.stages; ++s) {
        for (int p = 0; p < MAX_POSTS; ++p) {
            Send(&postPeer[s][p], M_STOP, 0, 0);
        }
        // машины, ждущие места в накопителе, проснутся и прочтут STOP из своего канала
        for (int k = 0; k < MAX_SLOTS; ++k) {
            sem_post(queueSem[s]);
        }
    }
    for (int i = 0; i < MAX_SLOTS; ++i) {
        Send(&carPeer[i], M_STOP, 0, 0);
    }
    Send(&sourcePeer, M_STOP, 0, 0);
    sem_post(openSem);
}

static void CheckDone(void) {
    if (st.startNs && st.closed && st.active == 0) {
        RequestStop(STOP_DONE);
    }
}

static void OpenEntranceIfReady(void) {
    if (st.startNs == 0 && postsHere == ConfigTotalPosts(&st.cfg) && sourcePeer.fd >= 0) {
        st.startNs = NowNs();
        OutEvent(0, "coordinator", getpid(), C_COORD,
                 "all %d posts and the car source are here, entrance opened", postsHere);
        sem_post(openSem);
    }
}

static void OnHello(const Msg *m) {
    if (stopping) {
        Peer tmp;
        Answer(&tmp, m->pid, 0);
        return;
    }
    if (m->kind == K_POST) {
        int s = -1, p = m->b;
        for (int i = 0; i < st.cfg.stages; ++i) {
            if (strncmp(st.cfg.stage[i].name, m->name, NAME_LEN) == 0) {
                s = i;
            }
        }
        int ok = s >= 0 && s < st.cfg.stages && p >= 0 && p < st.cfg.stage[s].posts &&
                 postPeer[s][p].fd < 0;
        if (!ok) {
            Peer tmp;
            Answer(&tmp, m->pid, 0);
            return;
        }
        if (Answer(&postPeer[s][p], m->pid, 1) == 0) {
            st.post[s][p].state = POST_IDLE;
            st.post[s][p].pid = m->pid;
            ++postsHere;
            dprintf(1, "post %s.%d connected (pid %d), posts %d/%d\n", st.cfg.stage[s].name, p + 1,
                    m->pid, postsHere, ConfigTotalPosts(&st.cfg));
        }
    } else if (m->kind == K_CARS) {
        if (sourcePeer.fd >= 0) {
            Peer tmp;
            Answer(&tmp, m->pid, 0);
            return;
        }
        if (Answer(&sourcePeer, m->pid, 1) == 0) {
            dprintf(1, "car source connected (pid %d)\n", m->pid);
        }
    } else if (m->kind == K_CAR && m->a >= 0 && m->a < MAX_SLOTS && st.car[m->a].state == CAR_NEW) {
        Answer(&carPeer[m->a], m->pid, 1);
    }
    OpenEntranceIfReady();
}

static void OnCarMessage(const Msg *m) {
    int slot = CarByPid(m->pid);
    if (slot < 0) {
        return;
    }
    CarSlot *c = &st.car[slot];
    switch (m->type) {
    case M_ARRIVE: {
        int program = m->a >= 0 && m->a < st.cfg.programs ? m->a : 0;
        if (StArrive(&st, slot, program, m->pid, m->b)) {
            Dispatch(st.cfg.program[program].route[0]);
        } else {
            PeerDrop(&carPeer[slot]);  // машина уехала, процесс завершается сам
        }
        break;
    }
    case M_BLOCKED: StBlocked(&st, slot); break;
    case M_MOVE: {
        int from = c->stage;
        int freed = StMoveOn(&st, slot);
        if (freed >= 0) {
            Send(&postPeer[from][freed], M_FREE, 0, 0);
        }
        Dispatch(st.cfg.program[c->program].route[c->step]);
        break;
    }
    case M_LEAVE: {
        int stage = c->stage;
        int post = StFinish(&st, slot);
        Send(&postPeer[stage][post], M_FREE, 0, 0);
        PeerDrop(&carPeer[slot]);
        break;
    }
    default: break;
    }
}

static void OnPostMessage(const Msg *m) {
    int s, p;
    if (PostByPid(m->pid, &s, &p) != 0) {
        return;
    }
    PostSlot *ps = &st.post[s][p];
    switch (m->type) {
    case M_OPSTART: StOpStart(&st, s, p, m->a); break;
    case M_OPEND: {
        int car = ps->car;
        StOpEnd(&st, s, p, m->a);
        if (car >= 0) {
            Send(&carPeer[car], M_DONE, 0, 0);
        }
        break;
    }
    case M_BREAK: StBreak(&st, s, p, m->a); break;
    case M_REPAIR: StRepair(&st, s, p); break;
    case M_IDLE:
        StPostFree(&st, s, p);
        Dispatch(s);
        break;
    default: break;
    }
}

static void OnSourceMessage(const Msg *m) {
    if (m->pid != sourcePeer.pid) {
        return;
    }
    if (m->type == M_NEWCAR) {
        if (st.cfg.day > 0 && StNow(&st) >= st.cfg.day) {
            StClose(&st, "end of the working day");
        }
        int slot = stopping ? -1 : StReserveCar(&st);
        if (slot < 0 && !st.closed) {
            OutEvent(StNow(&st), "entrance", getpid(), C_WAIT, "no room for one more car");
        }
        if (slot >= 0) {
            PeerReset(&carPeer[slot]);  // pid и канал машина пришлёт в HELLO
        }
        Send(&sourcePeer, M_SLOT, st.closed ? -1 : slot, slot >= 0 ? st.car[slot].id : 0);
    } else if (m->type == M_CLOSE) {
        char why[48];
        snprintf(why, sizeof(why), "all %d cars have arrived", m->a);
        StClose(&st, why);
    }
}

/// Участник отключился: убираем его из таблиц.
static void OnBye(const Msg *m) {
    int s, p;
    if (PostByPid(m->pid, &s, &p) == 0) {
        PeerDrop(&postPeer[s][p]);
        st.post[s][p].state = POST_OFF;
        --postsHere;
        if (!stopping) {
            RequestStop(STOP_FAILURE);  // без поста линия работать не может
        }
    } else if (m->pid == sourcePeer.pid) {
        PeerDrop(&sourcePeer);
    } else {
        int slot = CarByPid(m->pid);
        if (slot >= 0) {
            PeerDrop(&carPeer[slot]);
        }
    }
}

static void Handle(const Msg *m) {
    if (m->type == M_HELLO) {
        OnHello(m);
        return;
    }
    if (m->type == M_INTERRUPT) {
        RequestStop(STOP_INTERRUPT);
        return;
    }
    if (m->type == M_BYE) {
        OnBye(m);
        return;
    }
    if (stopping) {
        return;  // после остановки ждём только отключения участников
    }
    if (m->kind == K_CAR) {
        OnCarMessage(m);
    } else if (m->kind == K_POST) {
        OnPostMessage(m);
    } else if (m->kind == K_CARS) {
        OnSourceMessage(m);
    }
    CheckDone();
}

static void HandleSignals(void) {
    if (gotInterrupt) {
        gotInterrupt = 0;
        RequestStop(STOP_INTERRUPT);
    }
    if (gotStatus) {
        gotStatus = 0;
        StStatus(&st);
    }
    if (gotClose) {
        gotClose = 0;
        StClose(&st, "closed early by SIGUSR2");
        CheckDone();
    }
}

/// Кто-то ещё не отключился?
static int PeersLeft(void) {
    int n = postsHere + (sourcePeer.fd >= 0);
    for (int i = 0; i < MAX_SLOTS; ++i) {
        n += carPeer[i].fd >= 0;
    }
    return n;
}

/// Создаёт каталог, общий канал и семафоры. @return 0 или -1
static int CreateObjects(char *err, int errSize) {
    ProtoDir(key, dirPath, sizeof(dirPath));
    ProtoCoordPath(key, coordPath, sizeof(coordPath));
    if (mkdir(dirPath, 0700) != 0 && errno != EEXIST) {
        snprintf(err, errSize, "mkdir %s: %s", dirPath, strerror(errno));
        return -1;
    }
    // канал от прошлого прогона: если его кто-то читает, координатор уже запущен
    int probe = open(coordPath, O_WRONLY | O_NONBLOCK);
    if (probe >= 0) {
        close(probe);
        snprintf(err, errSize, "key '%s' is in use by a running coordinator", key);
        return -1;
    }
    unlink(coordPath);
    if (mkfifo(coordPath, 0600) != 0) {
        snprintf(err, errSize, "mkfifo %s: %s", coordPath, strerror(errno));
        return -1;
    }
    // O_RDWR: канал не получит конец файла, когда отключится последний участник
    coordFd = open(coordPath, O_RDWR);
    if (coordFd < 0) {
        snprintf(err, errSize, "open %s: %s", coordPath, strerror(errno));
        return -1;
    }
    char name[40];
    for (int s = 0; s < st.cfg.stages; ++s) {
        ProtoSemName(key, s, name, sizeof(name));
        sem_unlink(name);
        queueSem[s] = sem_open(name, O_CREAT | O_EXCL, 0600, (unsigned)st.cfg.stage[s].queue);
        if (queueSem[s] == SEM_FAILED) {
            snprintf(err, errSize, "sem_open %s: %s", name, strerror(errno));
            return -1;
        }
    }
    ProtoSemName(key, -1, name, sizeof(name));
    sem_unlink(name);
    openSem = sem_open(name, O_CREAT | O_EXCL, 0600, 0);
    if (openSem == SEM_FAILED) {
        snprintf(err, errSize, "sem_open %s: %s", name, strerror(errno));
        return -1;
    }
    return 0;
}

static void DestroyObjects(void) {
    char name[40];
    for (int s = 0; s < st.cfg.stages; ++s) {
        if (queueSem[s] && queueSem[s] != SEM_FAILED) {
            sem_close(queueSem[s]);
        }
        ProtoSemName(key, s, name, sizeof(name));
        sem_unlink(name);
    }
    if (openSem != SEM_FAILED) {
        sem_close(openSem);
    }
    ProtoSemName(key, -1, name, sizeof(name));
    sem_unlink(name);
    if (coordFd >= 0) {
        close(coordFd);
    }
    unlink(coordPath);
    rmdir(dirPath);  // каналы участников они удаляют сами; не пусто - оставляем
}

int main(int argc, char **argv) {
    Config cfg;
    if (ConfigFromArgs(&cfg, argc, argv, "hk:", OnExtra) != 0) {
        return 1;
    }
    int journal = open(cfg.journal, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    if (journal < 0) {
        dprintf(2, "%s: cannot open journal %s\n", argv[0], cfg.journal);
        return 1;
    }
    OutInit(journal, cfg.quiet);
    SignalsInstall();
    if (StateInit(&st, &cfg) != 0) {
        dprintf(2, "%s: the line holds too many cars (max %d)\n", argv[0], MAX_SLOTS);
        return 1;
    }
    for (int s = 0; s < MAX_STAGES; ++s) {
        for (int p = 0; p < MAX_POSTS; ++p) {
            PeerReset(&postPeer[s][p]);
        }
    }
    for (int i = 0; i < MAX_SLOTS; ++i) {
        PeerReset(&carPeer[i]);
    }
    PeerReset(&sourcePeer);

    char err[200];
    if (CreateObjects(err, sizeof(err)) != 0) {
        dprintf(2, "%s: %s\n", argv[0], err);
        DestroyObjects();
        return 5;
    }
    OutLine("Car wash, variant 43: independent processes, named POSIX semaphores, named pipes "
            "in %s (coordinator pid %d)",
            dirPath, (int)getpid());
    ConfigPrint(&cfg, 1);
    ConfigPrint(&cfg, journal);

    if (ConfigPossiblePrograms(&cfg) == 0) {
        OutEvent(0, "coordinator", getpid(), C_FAIL,
                 "no program can be served: every route "
                 "needs a stage without posts");
        RequestStop(STOP_IMPOSSIBLE);
    } else {
        dprintf(1, "waiting for %d post processes and the car source...\n", ConfigTotalPosts(&cfg));
    }

    // главный цикл: по одному сообщению из общего канала
    long long leaveDeadline = 0;
    for (;;) {
        HandleSignals();  // сигнал мог прийти вне select - тогда EINTR не будет
        if (stopping && leaveDeadline == 0) {
            leaveDeadline = NowNs() + LEAVE_TIMEOUT_MS * 1000000LL;
        }
        if (stopping && (PeersLeft() == 0 || NowNs() > leaveDeadline)) {
            break;
        }
        fd_set set;
        FD_ZERO(&set);
        FD_SET(coordFd, &set);
        struct timeval tv = {0, 100000};
        int r = select(coordFd + 1, &set, NULL, NULL, stopping ? &tv : NULL);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (r == 0) {
            continue;
        }
        Msg m;
        long got = ReadAll(coordFd, &m, sizeof(m));
        if (got == (long)sizeof(m)) {
            Handle(&m);
        }
    }

    StReport(&st, stopping);
    int code = StExitCode(&st, stopping);
    DestroyObjects();
    close(journal);
    return code;
}
