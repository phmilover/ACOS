/**
 * @file post.c
 * @brief Поток поста.
 *
 * Порядок дел за минуту как в ИЗ1 (UpdateEquipment, затем SweepStage): сначала ремонт или
 * возможная поломка, затем работа. Пост, починенный в эту минуту, в ней же и работает;
 * сломавшийся - нет (операции на нём замирают до ремонта).
 */

#include "post.h"

#include "events.h"
#include "line.h"
#include "soft_assert.h"
#include "stage.h"

/// Первая машина на посту (для текста поломки) или -1.
static int CarOnPost(const Post *post) {
    for (int k = 0; k < post->capacity; ++k) {
        if (post->seat[k].car >= 0) {
            return post->seat[k].car;
        }
    }
    return -1;
}

/// Пост сломался: машины, ждущие на нём конца операции, теперь ждут ремонта (для графа
/// ожидания). Машина, которая ещё не уснула, сама выберет WAIT_REPAIR (StageAwaitOperation).
static void MarkWaitRepair(Line *l, Post *post) {
    for (int k = 0; k < post->capacity; ++k) {
        const Seat *seat = &post->seat[k];
        if (seat->car < 0 || seat->left <= 0) {
            continue;  // свободно, бронь или машина уже помыта (она ждёт не этот пост)
        }
        Car *car = LineCar(l, seat->car);
        if (SOFT_ASSERT(car != NULL, "unknown car on a post")) {
            SimSetReason(
                &car->th,
                (WaitReason){.kind = WAIT_REPAIR, .stage = post->stage, .post = post->index});
        }
    }
}

/// Ремонт или возможная поломка. @return 1, если пост исправен и может работать
static int UpdateEquipment(Line *l, Stage *st, Post *post) REQUIRES(st->m) {
    const Config *cfg = l->cfg;
    if (post->repair > 0) {
        if (--post->repair == 0) {
            EV(EV_REPAIR, -1, post->stage, post->index, -1, 0);
            StageRepaired(l, st);  // починенный пост ждут машины очереди и других стадий
            return 1;
        }
        return 0;
    }
    // пост с бронью не ломается: машина уже едет на обещанное место
    if (post->reserved == 0 && cfg->failP > 0 && RngChance(&post->rng, cfg->failP)) {
        post->repair = RngRange(&post->rng, cfg->repairMin, cfg->repairMax);
        EV(EV_FAIL, CarOnPost(post), post->stage, post->index, -1, post->repair);
        atomic_fetch_add(&l->failures, 1);
        MarkWaitRepair(l, post);
        return 0;
    }
    return 1;
}

/// Минута работы: операции, начатые до этой минуты, идут дальше; закончилась - будим машины.
static void Work(Line *l, Stage *st, Post *post, int now) REQUIRES(st->m) {
    int ended = 0;
    for (int k = 0; k < post->capacity; ++k) {
        Seat *seat = &post->seat[k];
        if (seat->car < 0 || seat->left <= 0 || seat->start >= now) {
            continue;  // свободно, бронь, уже помыта или села только что
        }
        if (--seat->left > 0) {
            continue;
        }
        const Car *car = LineCar(l, seat->car);
        EV(EV_OP_END, seat->car, post->stage, post->index, car ? car->program : -1, 0);
        ended = 1;
    }
    if (ended) {
        StageWakePost(st, post);
    }
}

/// Саботаж SAB_RACE: общий счётчик без мьютекса. Посты одной минуты работают параллельно, так
/// что это гонка данных, которую должен поймать ThreadSanitizer.
static void RacyTick(Line *l) NO_TSA {
    l->raceTicks++;
}

void *PostMain(void *arg) {
    Post *post = arg;
    Line *l = post->line;
    Stage *st = &l->stage[post->stage];
    BarrierWait(&l->start);
    if (SimBegin(&post->th) == SIM_OK) {
        EventsSetSource(&post->th);
        // минута 0 - открытие: первая минута работы - первая
        while (SimSleep(&post->th, 1) == SIM_OK) {
            if (l->cfg->sabotage == SAB_RACE) {
                RacyTick(l);  // до мьютекса стадии: иначе он упорядочил бы посты одной стадии
            }
            int now = SimNow();
            MutexLock(&st->m);
            if (UpdateEquipment(l, st, post)) {
                Work(l, st, post, now);
            }
            MutexUnlock(&st->m);
        }
    }
    SimExit(&post->th);
    return NULL;
}
