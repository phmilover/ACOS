/**
 * @file main.c
 * @brief Автомойка, вариант 43, многопоточная на мониторах: поток на машину, пост и
 * координатора.
 *
 * Главный поток разбирает ключи и конфиг, создаёт посты, координатора и часы, и все они вместе
 * с ним проходят барьер старта (Line.start): модель начинается, только когда все участники
 * созданы. Затем main ждёт их завершения (join), собирает оставшиеся потоки машин и печатает
 * итоги.
 *
 * Коды выхода: 0 - всё вымыто, 1 - плохие параметры, 2 - тупик, 3 - прервали сигналом,
 * 4 - сработала проверка (LOCKDEP, SOFT_ASSERT), 5 - не хватило ресурсов (память, поток,
 * условная переменная). Не создался участник барьера старта - выход с кодом 5 сразу, без
 * итогов: барьер нельзя пройти без всех участников, а уже созданные ждут на нём.
 */

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "coordinator.h"
#include "events.h"
#include "line.h"
#include "post.h"
#include "quiesce.h"
#include "sim.h"
#include "soft_assert.h"
#include "thread.h"
#include "view_log.h"

static void Usage(const char *prog) {
    dprintf(1,
            "Automatic car wash simulation (variant 43), multithreaded\n"
            "Usage: %s [options]\n"
            "  -c FILE   configuration file (default: built-in typical car wash)\n"
            "  -s SEED   random seed\n"
            "  -d MS     display delay per model minute, ms\n"
            "  -m MODE   view: live (animated line) or log (event feed)\n"
            "  -l FILE   journal file (default: carwash.log)\n"
            "  -u        unlimited mode: no day end and no car limit, stop with Ctrl+C\n"
            "  -i        ask the main parameters interactively\n"
            "  -x KIND   sabotage: overflow | race | deadlock (inject a bug to prove the\n"
            "            checks catch it)\n"
            "  -D        deterministic: one model thread at a time, the same seed gives the\n"
            "            same journal\n"
            "  -t        show the thread that reported each event\n"
            "  -g FILE   write the wait-for graph in DOT on a stall\n"
            "  -h        this help\n"
            "Exit codes: 0 done, 1 bad parameters, 2 stall, 3 interrupted, 4 a check failed\n"
            "            (LOCKDEP or SOFT_ASSERT), 5 out of resources (memory, thread or\n"
            "            condition variable). LOCKDEP and out of memory exit at once, without\n"
            "            the results\n",
            prog);
}

/// Ключи командной строки; они важнее того, что написано в конфиге.
typedef struct {
    const char *cfgFile, *seedArg, *delayArg, *modeArg, *journalArg, *sabotageArg, *dotArg;
    int unlimited, interactive, deterministic, threadColumn;
} Options;

/// @return 0, 1 - ошибка (код выхода 1) или -1 - напечатана справка (код 0)
static int ParseOptions(int argc, char **argv, Options *o) {
    memset(o, 0, sizeof *o);
    int opt;
    // ключи разбираются, пока других потоков нет
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    while ((opt = getopt(argc, argv, "c:s:d:m:l:uix:Dtg:h")) != -1) {
        switch (opt) {
        case 'c': o->cfgFile = optarg; break;
        case 's': o->seedArg = optarg; break;
        case 'd': o->delayArg = optarg; break;
        case 'm': o->modeArg = optarg; break;
        case 'l': o->journalArg = optarg; break;
        case 'u': o->unlimited = 1; break;
        case 'i': o->interactive = 1; break;
        case 'x': o->sabotageArg = optarg; break;
        case 'D': o->deterministic = 1; break;
        case 't': o->threadColumn = 1; break;
        case 'g': o->dotArg = optarg; break;
        case 'h': Usage(argv[0]); return -1;
        default: Usage(argv[0]); return 1;
        }
    }
    if (optind < argc) {
        dprintf(2, "carwash-mon: unexpected argument '%s'\n", argv[optind]);
        return 1;
    }
    return 0;
}

/// Переносит ключи в конфиг. @return 0 или 1 (сообщение уже напечатано)
static int ApplyOptions(const Options *o, Config *cfg) {
    int number = 0;
    if (o->seedArg) {
        if (ParseInt(o->seedArg, &number) != 0) {
            dprintf(2, "carwash-mon: bad seed '%s'\n", o->seedArg);
            return 1;
        }
        cfg->seed = (unsigned)number;
        cfg->seedSet = 1;
    }
    if (o->delayArg) {
        if (ParseInt(o->delayArg, &number) != 0 || number < 0) {
            dprintf(2, "carwash-mon: bad delay '%s'\n", o->delayArg);
            return 1;
        }
        cfg->delayMs = number;
    }
    if (o->modeArg) {
        if (strcmp(o->modeArg, "live") == 0) {
            cfg->view = VIEW_LIVE;
        } else if (strcmp(o->modeArg, "log") == 0) {
            cfg->view = VIEW_LOG;
        } else {
            dprintf(2, "carwash-mon: unknown view mode '%s' (use live or log)\n", o->modeArg);
            return 1;
        }
    }
    if (o->sabotageArg && ConfigParseSabotage(o->sabotageArg, &cfg->sabotage) != 0) {
        dprintf(2, "carwash-mon: unknown sabotage '%s' (use overflow, race or deadlock)\n",
                o->sabotageArg);
        return 1;
    }
    if (o->journalArg) {
        snprintf(cfg->journal, sizeof(cfg->journal), "%s", o->journalArg);
    }
    if (o->dotArg) {
        snprintf(cfg->dotFile, sizeof(cfg->dotFile), "%s", o->dotArg);
    }
    if (o->unlimited) {
        cfg->day = cfg->cars = 0;
    }
    cfg->deterministic = cfg->deterministic || o->deterministic;
    cfg->threadColumn = cfg->threadColumn || o->threadColumn;
    return 0;
}

static const char *Outcome(LineStatus status) {
    switch (status) {
    case LINE_FINISHED: return "finished: entrance closed and every accepted car washed";
    case LINE_STALLED: return "STALLED: remaining cars can never be served";
    case LINE_INTERRUPTED: return "INTERRUPTED by user";
    case LINE_NO_RESOURCES: return "STOPPED: not enough resources (thread or condition variable)";
    default: return "running";
    }
}

/// Итоги прогона (пока исход и счётчики; полная статистика - stats.c).
static void PrintResults(int fd, Line *l, int now) {
    char from[16], to[16];
    EventClock(0, from, sizeof(from));
    EventClock(now, to, sizeof(to));
    RwLockRead(&l->registry);
    int arrived = l->carCount;
    RwLockUnlock(&l->registry);
    int peak = atomic_load(&l->peakAlive);
    int rejected = atomic_load(&l->rejected), served = atomic_load(&l->served);
    int inside = atomic_load(&l->inside), failures = atomic_load(&l->failures);

    dprintf(fd, "\n=================== RESULTS ===================\n");
    dprintf(fd, "Outcome        : %s\n", Outcome(l->status));
    dprintf(fd, "Model time     : %s - %s (%d min)\n", from, to, now);
    dprintf(fd, "Cars arrived   : %d (accepted %d, turned away %d)\n", arrived, arrived - rejected,
            rejected);
    dprintf(fd, "Cars washed    : %d, still inside: %d\n", served, inside);
    dprintf(fd, "Failures       : %d\n", failures);
    dprintf(fd, "Car threads    : %d, at most %d at once\n", arrived, peak);
}

/// Нарушение (LOCKDEP в нефатальном режиме, SOFT_ASSERT) важнее исхода модели.
static int ExitCode(const Line *l) {
    return LineExitCode(l->status, 0, LockdepViolations(), SoftAssertFailures());
}

/// Собрана ли программа с ThreadSanitizer (сборка -tsan).
static int UnderTsan(void) {
#ifdef __has_feature
#if __has_feature(thread_sanitizer)
    return 1;
#endif
#endif
#ifdef __SANITIZE_THREAD__
    return 1;
#else
    return 0;
#endif
}

/// Сколько участников у барьера старта: посты, координатор, часы и сам main.
static int StartParticipants(const Config *cfg) {
    int posts = 0;
    for (int s = 0; s < cfg->stages; ++s) {
        posts += cfg->stage[s].posts;
    }
    return posts + 3;
}

/// Не создался участник барьера старта: уже созданные ждут на барьере, и пройти его без
/// недостающего нельзя - выходим сразу, без итогов.
_Noreturn static void StartFailed(void) {
    dprintf(2, "carwash-mon: not enough resources: cannot create a thread\n");
    exit(5);  // NOLINT(concurrency-mt-unsafe): остальные потоки модели ещё на барьере
}

/// Создаёт поток модели (SimSpawned до ThreadCreate) или выходит с кодом 5.
static void Spawn(SimThread *th, pthread_t *tid, void *(*fn)(void *), void *arg) {
    SimSpawned(th);
    if (ThreadCreate(tid, fn, arg, 0) != 0) {
        StartFailed();
    }
}

/// Собирает все потоки модели: посты, координатора, затем машины из реестра.
static void JoinAll(Line *l) {
    for (int s = 0; s < l->cfg->stages; ++s) {
        for (int p = 0; p < l->cfg->stage[s].posts; ++p) {
            pthread_join(l->stage[s].post[p].tid, NULL);
        }
    }
    pthread_join(l->coordTid, NULL);
    // координатора уже нет: машин больше не прибавится, и joined никто не пишет
    Car *car;
    for (int i = 0; (car = LineCar(l, i)) != NULL; ++i) {
        if (!car->joined) {
            pthread_join(car->tid, NULL);
            car->joined = 1;
        }
    }
    atomic_store(&l->alive, 0);
}

static void NoResources(void) {
    dprintf(2, "carwash-mon: not enough resources: cannot create a thread or a condition "
               "variable\n");
}

/// Запускает модель и ждёт её конца. Порядок создания не важен: часы и потоки модели начинают
/// работу только после барьера старта, который main проходит последним из создателей.
static void RunModel(Line *l) {
    const Config *cfg = l->cfg;
    for (int s = 0; s < cfg->stages; ++s) {
        for (int p = 0; p < cfg->stage[s].posts; ++p) {
            Post *post = &l->stage[s].post[p];
            Spawn(&post->th, &post->tid, PostMain, post);
        }
    }
    Spawn(&l->coordTh, &l->coordTid, CoordinatorMain, l);
    pthread_t clock;
    if (ThreadCreate(&clock, SimClockMain, NULL, 0) != 0) {
        StartFailed();
    }
    BarrierWait(&l->start);
    pthread_join(clock, NULL);
    JoinAll(l);
}

int main(int argc, char **argv) {
    Options o;
    int rc = ParseOptions(argc, argv, &o);
    if (rc != 0) {
        return rc < 0 ? 0 : 1;
    }
    // `carwash-mon -m log | head`: закрытый канал не убивает процесс, write вернёт EPIPE, а
    // итоги и журнал всё равно сохранятся
    struct sigaction ignore;
    memset(&ignore, 0, sizeof ignore);
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGPIPE, &ignore, NULL);

    static Config cfg;
    char err[256];
    ConfigDefaults(&cfg);
    rc = o.cfgFile ? ConfigLoadFile(&cfg, o.cfgFile, err, sizeof(err))
                   : ConfigParseText(&cfg, CONFIG_BUILTIN, err, sizeof(err));
    if (ApplyOptions(&o, &cfg) != 0) {
        return 1;
    }
    if (rc == 0 && o.interactive) {
        ConfigInteractive(&cfg);
    }
    if (rc != 0 || ConfigValidate(&cfg, err, sizeof(err)) != 0) {
        dprintf(2, "carwash-mon: configuration error: %s\n", err);
        return 1;
    }
    if (!cfg.seedSet) {
        cfg.seed = (unsigned)time(NULL) ^ (unsigned)getpid();
    }
    if (cfg.sabotage == SAB_RACE && !UnderTsan()) {
        dprintf(2, "carwash-mon: race sabotage is only visible in the -tsan build "
                   "(ThreadSanitizer)\n");
    }

    // живая схема появится позже (render/view_live); пока в любом режиме - лента
    int tty = isatty(1);
    ViewMode view = cfg.view == VIEW_AUTO ? (tty ? VIEW_LIVE : VIEW_LOG) : cfg.view;
    int delay = cfg.delayMs >= 0 ? cfg.delayMs : (view == VIEW_LIVE ? 150 : 0);

    static Line line;
    // часы ждут на барьере старта вместе с постами, координатором и main
    SimOptions so = {.deterministic = cfg.deterministic,
                     .delayMs = delay,
                     .hook = QuiesceHook,
                     .ctx = &line,
                     .start = &line.start};
    if (SimInit(&so) != 0) {
        NoResources();
        return 5;
    }
    if (LineInit(&line, &cfg) != 0) {
        NoResources();
        return 5;
    }
    line.barrierThreads = StartParticipants(&cfg);
    if (BarrierInit(&line.start, (unsigned)line.barrierThreads) != 0) {
        NoResources();
        LineFree(&line);
        return 5;
    }

    int journal = open(cfg.journal, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (journal == -1) {
        perror("carwash-mon: cannot open journal, continuing without it");
    }
    EventsInit(&cfg);
    ViewLogInit(1, tty, journal, cfg.threadColumn);
    EventsSubscribe(ViewLogListener);
    dprintf(1, "=== AUTOMATIC CAR WASH: simulation of one working day ===\n");
    ConfigPrint(&cfg, 1);
    dprintf(1, "Mode: %s\n\n", cfg.deterministic ? "deterministic (-D)" : "threads in parallel");
    if (journal != -1) {
        dprintf(journal, "=== AUTOMATIC CAR WASH: journal ===\n");
        ConfigPrint(&cfg, journal);
        dprintf(journal, "\n");
    }

    RunModel(&line);
    if (line.status == LINE_NO_RESOURCES) {
        NoResources();
    }
    int now = SimNow();
    PrintResults(1, &line, now);
    if (journal != -1) {
        PrintResults(journal, &line, now);
        dprintf(journal, "seed %u (repeat the run with -s %u)\n", cfg.seed, cfg.seed);
        close(journal);
        dprintf(1, "Journal saved to %s (seed %u)\n", cfg.journal, cfg.seed);
    }
    if (SoftAssertFailures() > 0) {
        dprintf(2, "carwash-mon: soft asserts failed %ld time(s), see messages above\n",
                SoftAssertFailures());
    }

    int code = ExitCode(&line);
    BarrierDestroy(&line.start);
    LineFree(&line);
    return code;
}
