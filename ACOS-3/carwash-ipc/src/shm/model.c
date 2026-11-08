#include "model.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include "util.h"

#define SLEEP_CHUNK_MS 20  // как часто спящий процесс проверяет, не пора ли остановиться

void MLock(Ipc *ipc) {
    // сигнал не должен отменить захват: обработаем его на следующем ожидании
    while (sem_wait(ipc->sem[SEM_MUTEX]) != 0 && errno == EINTR) {
    }
}

void MUnlock(Ipc *ipc) {
    sem_post(ipc->sem[SEM_MUTEX]);
}

void MSignal(Ipc *ipc, int sem) {
    sem_post(ipc->sem[sem]);
}

void MRequestStop(Ipc *ipc, StopReason why) {
    int expected = STOP_NONE;
    atomic_compare_exchange_strong(&ipc->sh->stop, &expected, (int)why);
    MSignal(ipc, SEM_COORD);
}

static int Stopping(const Ipc *ipc) {
    return atomic_load(&ipc->sh->stop) != STOP_NONE;
}

void MHandleSignals(Ipc *ipc) {
    if (gotInterrupt) {
        gotInterrupt = 0;
        MRequestStop(ipc, STOP_INTERRUPT);
    }
    if (gotClose) {
        gotClose = 0;
        MLock(ipc);
        StClose(&ipc->sh->st, "closed early by SIGUSR2");
        MUnlock(ipc);
        MSignal(ipc, SEM_COORD);
    }
    if (gotStatus) {
        gotStatus = 0;
        MLock(ipc);
        StStatus(&ipc->sh->st);
        MUnlock(ipc);
    }
}

int MWait(Ipc *ipc, int sem) {
    for (;;) {
        // флаг проверяем на каждом витке: сигнал мог прийти вне sem_wait, и тогда EINTR не будет
        MHandleSignals(ipc);
        if (Stopping(ipc)) {
            return -1;
        }
        if (sem_wait(ipc->sem[sem]) == 0) {
            return Stopping(ipc) ? -1 : 0;
        }
        if (errno != EINTR) {
            MRequestStop(ipc, STOP_FAILURE);
            return -1;
        }
    }
}

int MSleep(Ipc *ipc, int minutes) {
    long long until = NowNs() + (long long)minutes * ipc->sh->st.cfg.delayMs * 1000000LL;
    for (;;) {
        MHandleSignals(ipc);
        if (Stopping(ipc)) {
            return -1;
        }
        long long left = (until - NowNs()) / 1000000LL;
        if (left <= 0) {
            return 0;
        }
        SleepMs(left < SLEEP_CHUNK_MS ? (int)left : SLEEP_CHUNK_MS);
    }
}

void MWakeAll(Ipc *ipc) {
    const Config *cfg = &ipc->sh->st.cfg;
    for (int s = 0; s < cfg->stages; ++s) {
        for (int p = 0; p < MAX_POSTS; ++p) {
            MSignal(ipc, SEM_WORK(s));
            MSignal(ipc, SEM_POST(s, p));
        }
        // в накопитель стадии могут ждать места сколько угодно машин - разбудим всех
        for (int k = 0; k < MAX_SLOTS; ++k) {
            MSignal(ipc, SEM_QUEUE(s));
        }
    }
    for (int c = 0; c < MAX_SLOTS; ++c) {
        MSignal(ipc, SEM_CAR(c));
    }
    MSignal(ipc, SEM_OPEN);
    MSignal(ipc, SEM_COORD);
}

int MCar(Ipc *ipc, int slot) {
    State *st = &ipc->sh->st;
    const Config *cfg = &st->cfg;
    Rng rng;
    RngInit(&rng, cfg->seed, 1000U + (unsigned)st->car[slot].id);
    int program = StChooseProgram(cfg, &rng);
    const ProgramSpec *prog = &cfg->program[program];
    int first = prog->route[0];

    // место в первой очереди берём, только если программа вообще выполнима
    MLock(ipc);
    int gotPlace = ConfigDeadStage(cfg, program) < 0 && cfg->stage[first].queue > 0 &&
                   sem_trywait(ipc->sem[SEM_QUEUE(first)]) == 0;
    int accepted = StArrive(st, slot, program, getpid(), gotPlace);
    MUnlock(ipc);
    if (!accepted) {
        MSignal(ipc, SEM_COORD);
        return 0;
    }
    MSignal(ipc, SEM_WORK(first));

    for (;;) {
        // пост сам переводит машину к себе (StDispatch) и будит её дважды:
        // «заезжай» и «операция закончена»
        if (MWait(ipc, SEM_CAR(slot)) != 0 || MWait(ipc, SEM_CAR(slot)) != 0) {
            return 3;
        }
        MLock(ipc);
        CarSlot *c = &st->car[slot];
        int stage = c->stage;
        if (c->step + 1 == prog->len) {
            int post = StFinish(st, slot);
            MUnlock(ipc);
            MSignal(ipc, SEM_POST(stage, post));
            MSignal(ipc, SEM_COORD);
            return 0;
        }
        int next = prog->route[c->step + 1];
        if (cfg->stage[next].queue > 0 && sem_trywait(ipc->sem[SEM_QUEUE(next)]) != 0) {
            // места в накопителе нет: стоим на посту и держим его, пока место не появится
            StBlocked(st, slot);
            MUnlock(ipc);
            if (MWait(ipc, SEM_QUEUE(next)) != 0) {
                return 3;
            }
            MLock(ipc);
        }
        int freed = StMoveOn(st, slot);
        MUnlock(ipc);
        if (freed >= 0) {
            MSignal(ipc, SEM_POST(stage, freed));
        }
        MSignal(ipc, SEM_WORK(next));
    }
}

int MPost(Ipc *ipc, int stage, int post) {
    State *st = &ipc->sh->st;
    const Config *cfg = &st->cfg;
    const StageSpec *spec = &cfg->stage[stage];
    PostSlot *me = &st->post[stage][post];
    Rng rng;
    RngInit(&rng, cfg->seed, 100U + (unsigned)(stage * MAX_POSTS + post));

    MLock(ipc);
    me->pid = getpid();
    me->state = POST_IDLE;
    me->car = -1;
    ++ipc->sh->posts;
    MUnlock(ipc);
    MSignal(ipc, SEM_COORD);

    for (;;) {
        if (MWait(ipc, SEM_WORK(stage)) != 0) {
            break;
        }
        MLock(ipc);
        int queueFreed, prevStage, prevPost;
        int car = StDispatch(st, stage, post, &queueFreed, &prevStage, &prevPost);
        if (car < 0) {
            StCheck(st, 0, "post woke up, but no car waits for it");
            MUnlock(ipc);
            continue;
        }
        if (queueFreed) {
            MSignal(ipc, SEM_QUEUE(stage));  // под мьютексом: счётчик места и queueLen вместе
        }
        int minutes = RngRange(&rng, spec->tmin, spec->tmax);
        StOpStart(st, stage, post, minutes);
        MUnlock(ipc);
        if (prevPost >= 0) {
            MSignal(ipc, SEM_POST(prevStage, prevPost));
        }
        MSignal(ipc, SEM_CAR(car));  // заезжай

        if (MSleep(ipc, minutes) != 0) {
            break;
        }
        MLock(ipc);
        StOpEnd(st, stage, post, minutes);
        MUnlock(ipc);
        MSignal(ipc, SEM_CAR(car));  // операция закончена

        if (MWait(ipc, SEM_POST(stage, post)) != 0) {  // ждём, пока машина съедет
            break;
        }
        MLock(ipc);
        StPostFree(st, stage, post);
        if (RngChance(&rng, cfg->failP)) {
            int repair = RngRange(&rng, cfg->repairMin, cfg->repairMax);
            StBreak(st, stage, post, repair);
            MUnlock(ipc);
            if (MSleep(ipc, repair) != 0) {
                break;
            }
            MLock(ipc);
            StRepair(st, stage, post);
        }
        MUnlock(ipc);
    }

    MLock(ipc);
    me->state = POST_OFF;
    --ipc->sh->posts;
    MUnlock(ipc);
    MSignal(ipc, SEM_COORD);
    return 0;
}

void MArrivals(Ipc *ipc, SpawnCar spawn) {
    State *st = &ipc->sh->st;
    const Config *cfg = &st->cfg;
    Rng rng;
    RngInit(&rng, cfg->seed, 1);
    int spawned = 0;
    for (;;) {
        if (MSleep(ipc, RngRange(&rng, cfg->arrMin, cfg->arrMax)) != 0) {
            break;
        }
        MLock(ipc);
        if (cfg->day > 0 && StNow(st) >= cfg->day) {
            StClose(st, "end of the working day");
        }
        if (st->closed) {
            MUnlock(ipc);
            break;
        }
        int slot = StReserveCar(st);
        if (slot < 0) {
            OutEvent(StNow(st), "entrance", getpid(), C_WAIT, "no room for one more car");
        }
        MUnlock(ipc);
        if (slot >= 0 && spawn(ipc, slot) != 0) {
            MLock(ipc);
            StReleaseCar(st, slot);
            OutEvent(StNow(st), "entrance", getpid(), C_FAIL, "cannot start a car process");
            MUnlock(ipc);
            MRequestStop(ipc, STOP_FAILURE);
            break;
        }
        if (cfg->cars > 0 && ++spawned >= cfg->cars) {
            char why[48];
            snprintf(why, sizeof(why), "all %d cars have arrived", cfg->cars);
            MLock(ipc);
            StClose(st, why);
            MUnlock(ipc);
            break;
        }
    }
    MSignal(ipc, SEM_COORD);
}

StopReason MAwaitEnd(Ipc *ipc) {
    State *st = &ipc->sh->st;
    for (;;) {
        MHandleSignals(ipc);
        MLock(ipc);
        int done = st->closed && st->active == 0;
        MUnlock(ipc);
        int stop = atomic_load(&ipc->sh->stop);
        if (stop != STOP_NONE) {
            return (StopReason)stop;
        }
        if (done) {
            MRequestStop(ipc, STOP_DONE);
            return STOP_DONE;
        }
        sem_wait(ipc->sem[SEM_COORD]);  // EINTR - сигнал, флаги проверим в начале витка
    }
}

int MAttach(Ipc *ipc, const char *key, int seconds, const char *prog) {
    char err[200];
    for (int tries = 0;; ++tries) {
        if (IpcAttach(ipc, key, err, sizeof(err)) == 0) {
            if (kill(ipc->sh->coordPid, 0) == 0) {
                return 0;
            }
            // объекты остались от упавшего прогона: ждём, пока новый координатор их пересоздаст
            snprintf(err, sizeof(err), "coordinator pid %d is not running", ipc->sh->coordPid);
            IpcDetach(ipc);
        }
        if (tries == 0) {
            dprintf(2, "%s: waiting for the coordinator (key '%s')...\n", prog, key);
        }
        if (gotInterrupt || tries >= seconds * 5) {
            dprintf(2, "%s: %s\n", prog, err);
            return -1;
        }
        SleepMs(200);
    }
}

int MOpenJournal(Ipc *ipc) {
    int fd = open(ipc->sh->journal, O_WRONLY | O_APPEND);
    if (fd < 0) {
        dprintf(2, "cannot open journal %s\n", ipc->sh->journal);
        return -1;
    }
    OutInit(fd, ipc->sh->st.cfg.quiet);
    return 0;
}
