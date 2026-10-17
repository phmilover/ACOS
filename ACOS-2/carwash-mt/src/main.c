/**
 * @file main.c
 * @brief Автомойка, вариант 43, многопоточная: поток на машину, пост и координатора.
 *
 * Главный поток разбирает ключи и конфиг, блокирует сигналы (их забирает поток sigwait,
 * signals.h), запускает посты, координатора и часы, ждёт их завершения (join), собирает
 * оставшиеся потоки машин и печатает итоги.
 *
 * В режиме live схему рисует отдельный поток отрисовки (render.h), лента идёт только в журнал.
 *
 * Коды выхода: 0 - всё вымыто, 1 - плохие параметры, 2 - тупик, 3 - прервали сигналом,
 * 4 - нарушение инварианта, 5 - не хватило ресурсов (память, поток, семафор).
 */

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "auditor.h"
#include "config.h"
#include "coordinator.h"
#include "events.h"
#include "gantt.h"
#include "line.h"
#include "post.h"
#include "quiesce.h"
#include "render.h"
#include "signals.h"
#include "sim.h"
#include "soft_assert.h"
#include "stats.h"
#include "view_live.h"
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
            "Signals: SIGINT/SIGTERM interrupt (results and journal are saved), SIGUSR1 prints\n"
            "         the wait-for graph and threads to stderr, SIGUSR2 closes the entrance early\n"
            "Exit codes: 0 done, 1 bad parameters, 2 stall, 3 interrupted, 4 invariant violated,\n"
            "            5 out of resources (memory, thread or semaphore)\n",
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
        dprintf(2, "carwash-mt: unexpected argument '%s'\n", argv[optind]);
        return 1;
    }
    return 0;
}

/// Переносит ключи в конфиг. @return 0 или 1 (сообщение уже напечатано)
static int ApplyOptions(const Options *o, Config *cfg) {
    int number = 0;
    if (o->seedArg) {
        if (ParseInt(o->seedArg, &number) != 0) {
            dprintf(2, "carwash-mt: bad seed '%s'\n", o->seedArg);
            return 1;
        }
        cfg->seed = (unsigned)number;
        cfg->seedSet = 1;
    }
    if (o->delayArg) {
        if (ParseInt(o->delayArg, &number) != 0 || number < 0) {
            dprintf(2, "carwash-mt: bad delay '%s'\n", o->delayArg);
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
            dprintf(2, "carwash-mt: unknown view mode '%s' (use live or log)\n", o->modeArg);
            return 1;
        }
    }
    if (o->sabotageArg && ConfigParseSabotage(o->sabotageArg, &cfg->sabotage) != 0) {
        dprintf(2, "carwash-mt: unknown sabotage '%s' (use overflow, race or deadlock)\n",
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

/// Итоги прогона: статистика, диаграмма Ганта, сводка аудитора (как в ИЗ1).
static void PrintResults(int fd, Line *l, int color) {
    StatsPrint(fd, l);
    GanttPrint(fd, l->cfg, color);
    AuditorPrint(fd);
}

/// Нарушение инварианта (аудитор или LOCKDEP в нефатальном режиме) важнее исхода модели.
static int ExitCode(const Line *l) {
    if (AuditorViolations() > 0 || LockdepViolations() > 0) {
        return 4;
    }
    switch (l->status) {
    case LINE_FINISHED: return 0;
    case LINE_STALLED: return 2;
    case LINE_NO_RESOURCES: return 5;
    default: return 3;
    }
}

/// Потоки постов и координатора: SimSpawned, затем pthread_create.
/// @return 0 или -1 (созданные до ошибки отмечены в started)
static int StartThreads(Line *l, int started[MAX_STAGES][MAX_POSTS], int *coordStarted) {
    const Config *cfg = l->cfg;
    for (int s = 0; s < cfg->stages; ++s) {
        for (int p = 0; p < cfg->stage[s].posts; ++p) {
            Post *post = &l->stage[s].post[p];
            SimSpawned(&post->th);
            if (pthread_create(&post->tid, NULL, PostMain, post) != 0) {
                SimExit(&post->th);
                return -1;
            }
            started[s][p] = 1;
        }
    }
    SimSpawned(&l->coordTh);
    if (pthread_create(&l->coordTid, NULL, CoordinatorMain, l) != 0) {
        SimExit(&l->coordTh);
        return -1;
    }
    *coordStarted = 1;
    return 0;
}

/// Собирает все потоки модели: посты, координатора, затем машины из реестра.
static void JoinAll(Line *l, int started[MAX_STAGES][MAX_POSTS], int coordStarted) {
    for (int s = 0; s < l->cfg->stages; ++s) {
        for (int p = 0; p < l->cfg->stage[s].posts; ++p) {
            if (started[s][p]) {
                pthread_join(l->stage[s].post[p].tid, NULL);
            }
        }
    }
    if (coordStarted) {
        pthread_join(l->coordTid, NULL);
    }
    // координатора уже нет: машин больше не прибавится, и joined никто не пишет
    Car *car;
    for (int i = 0; (car = LineCar(l, i)) != NULL; ++i) {
        if (!car->joined) {
            pthread_join(car->tid, NULL);
            car->joined = 1;
        }
    }
    MutexLock(&l->registry);
    l->alive = 0;
    MutexUnlock(&l->registry);
}

static void NoResources(void) {
    dprintf(2, "carwash-mt: not enough resources: cannot create a thread or a semaphore\n");
}

/// Запускает модель и ждёт её конца. @return 0 или -1 (не хватило ресурсов при старте)
static int RunModel(Line *l) {
    int started[MAX_STAGES][MAX_POSTS] = {{0}};
    int coordStarted = 0;
    int rc = StartThreads(l, started, &coordStarted);
    pthread_t clock;
    // часы - последними (контракт sim.h): иначе они увидят покой до появления потоков
    if (rc != 0 || pthread_create(&clock, NULL, SimClockMain, NULL) != 0) {
        SimStop();  // уже запущенные потоки увидят остановку и выйдут
        JoinAll(l, started, coordStarted);
        return -1;
    }
    pthread_join(clock, NULL);
    JoinAll(l, started, coordStarted);
    return 0;
}

int main(int argc, char **argv) {
    Options o;
    int rc = ParseOptions(argc, argv, &o);
    if (rc != 0) {
        return rc < 0 ? 0 : 1;
    }
    // до любого другого потока: маску наследуют все, сигналы достанутся только потоку sigwait
    SignalsBlock();

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
        dprintf(2, "carwash-mt: configuration error: %s\n", err);
        return 1;
    }
    if (!cfg.seedSet) {
        cfg.seed = (unsigned)time(NULL) ^ (unsigned)getpid();
    }

    int tty = isatty(1);
    ViewMode view = cfg.view == VIEW_AUTO ? (tty ? VIEW_LIVE : VIEW_LOG) : cfg.view;
    int delay = cfg.delayMs >= 0 ? cfg.delayMs : (view == VIEW_LIVE ? 150 : 0);

    SimOptions so = {.deterministic = cfg.deterministic, .delayMs = delay, .hook = QuiesceHook};
    static Line line;
    so.ctx = &line;
    if (SimInit(&so) != 0) {
        NoResources();
        return 5;
    }
    if (LineInit(&line, &cfg) != 0) {
        NoResources();
        SimShutdown();
        return 5;
    }
    // после LineInit: он обнуляет line.requests, куда пишет поток сигналов. Сигнал, пришедший
    // раньше, не теряется - он заблокирован и ждёт sigwait
    if (SignalsStart(&line) != 0) {
        NoResources();
        LineFree(&line);
        SimShutdown();
        return 5;
    }

    int journal = open(cfg.journal, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (journal == -1) {
        perror("carwash-mt: cannot open journal, continuing without it");
    }
    EventsInit(&cfg);
    int live = view == VIEW_LIVE;
    // в живой схеме лента на экран не идёт, только в журнал; схему рисует поток отрисовки
    ViewLogInit(live ? -1 : 1, tty, journal, cfg.threadColumn);
    EventsSubscribe(ViewLogListener);
    if (live) {
        EventsSubscribe(ViewLiveListener);
    }
    AuditorInit(2, &cfg);
    EventsSubscribe(AuditorListener);
    EventsSubscribe(StatsListener);
    if (live && RenderStart() != 0) {
        NoResources();
        SignalsStop();
        if (journal != -1) {
            close(journal);
        }
        AuditorFree();
        StatsFree();
        GanttFree();
        LineFree(&line);
        SimShutdown();
        return 5;
    }
    dprintf(1, "=== AUTOMATIC CAR WASH: simulation of one working day ===\n");
    ConfigPrint(&cfg, 1);
    dprintf(1, "Mode: %s\n\n", cfg.deterministic ? "deterministic (-D)" : "threads in parallel");
    if (journal != -1) {
        dprintf(journal, "=== AUTOMATIC CAR WASH: journal ===\n");
        ConfigPrint(&cfg, journal);
        dprintf(journal, "\n");
    }

    if (live) {
        ViewLiveInit(1, &cfg);
        LockdepSetExitHook(ViewLiveEnd);  // LOCKDEP выходит через _exit: курсор вернуть и там
    }
    if (RunModel(&line) != 0) {
        line.status = LINE_NO_RESOURCES;
    }
    // последний кадр дорисовывается, затем курсор и цвета возвращаются на любом исходе
    RenderStop();
    ViewLiveEnd();
    SignalsStop();  // модель кончилась: дальнейшие сигналы ничего не меняют
    if (line.status == LINE_NO_RESOURCES) {
        NoResources();
    }
    PrintResults(1, &line, tty);
    if (journal != -1) {
        PrintResults(journal, &line, 0);
        dprintf(journal, "seed %u (repeat the run with -s %u)\n", cfg.seed, cfg.seed);
        close(journal);
        dprintf(1, "Journal saved to %s (seed %u)\n", cfg.journal, cfg.seed);
    }
    if (SoftAssertFailures() > 0) {
        dprintf(2, "carwash-mt: soft asserts failed %ld time(s), see messages above\n",
                SoftAssertFailures());
    }

    int code = ExitCode(&line);
    AuditorFree();
    StatsFree();
    GanttFree();
    LineFree(&line);
    SimShutdown();
    return code;
}
