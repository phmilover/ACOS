// Сценарные тесты: запускаем bin/carwash-mt на файлах из data/ и проверяем
// код выхода и вывод. Перенос из ИЗ1 (ACOS-1/carwash/tests/run_tests.c). Запуск из корня
// проекта:
//   cmake --build build --target check
// или вручную: ./bin/run_tests ./bin/carwash-mt
//
// У каждого прогона есть предел времени (RUN_TIMEOUT_S): зависшую программу тестировщик
// убивает и считает это провалом, а не ждёт вечно.

#include <dirent.h>
#include <fcntl.h>
#include <regex.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_ARGS      32
#define RUN_TIMEOUT_S 20  // обычный предел одного прогона

static const char *bin = "./bin/carwash-mt";
static char tmpDir[64];
static int passed = 0, failed = 0;
static int color = 0;

static void Report(int ok, const char *name, const char *what) {
    const char *mark = ok ? "PASS" : "FAIL";
    if (color) {
        printf("  %s%s\033[0m %s: %s\n", ok ? "\033[32m" : "\033[31m", mark, name, what);
    } else {
        printf("  %s %s: %s\n", mark, name, what);
    }
    ok ? ++passed : ++failed;
}

static void TmpPath(char *buf, size_t size, const char *name, const char *ext) {
    snprintf(buf, size, "%s/%s.%s", tmpDir, name, ext);
}

// Читает файл целиком; вызывающий освобождает память
static char *ReadFile(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        return NULL;
    }
    size_t size = 0, cap = 4096;
    char *buf = malloc(cap);
    ssize_t got;
    while (buf && (got = read(fd, buf + size, cap - size - 1)) > 0) {
        size += (size_t)got;
        if (cap - size < 2) {
            cap *= 2;
            char *bigger = realloc(buf, cap);
            if (bigger == NULL) {
                free(buf);
            }
            buf = bigger;  // при ошибке NULL, и цикл закончится
        }
    }
    close(fd);
    if (buf) {
        buf[size] = '\0';
    }
    return buf;
}

// Запускает carwash-mt -m log -l <tmp>/<name>.log <args...>, вывод идёт в <tmp>/<name>.out.
// env - "ИМЯ=значение" для дочернего процесса или NULL. Возвращает pid; дождаться - Wait()
static pid_t StartEnv(const char *name, const char *env, const char *const args[]) {
    char out[256], log[256];
    TmpPath(out, sizeof(out), name, "out");
    TmpPath(log, sizeof(log), name, "log");

    const char *argv[MAX_ARGS] = {bin, "-m", "log", "-l", log};
    int n = 5;
    for (int i = 0; args[i] != NULL && n < MAX_ARGS - 1; ++i) {
        argv[n++] = args[i];
    }
    argv[n] = NULL;

    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
        if (env) {
            putenv((char *)env);  // NOLINT(concurrency-mt-unsafe): дочерний процесс, один поток
        }
        execv(bin, (char *const *)argv);
        perror("execv");
        _exit(127);
    }
    return pid;
}

static pid_t Start(const char *name, const char *const args[]) {
    return StartEnv(name, NULL, args);
}

// Ждёт процесс не дольше seconds секунд. @return код выхода, -1 - убит сигналом,
// -2 - не успел (тогда процесс убит)
static int WaitFor(pid_t pid, int seconds) {
    struct timespec start, now, tick = {.tv_sec = 0, .tv_nsec = 10000000L};
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        int status = 0;
        pid_t got = waitpid(pid, &status, WNOHANG);
        if (got == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        if (got == -1) {
            return -1;
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec >= seconds) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -2;
        }
        nanosleep(&tick, NULL);
    }
}

static int Wait(pid_t pid) {
    return WaitFor(pid, RUN_TIMEOUT_S);
}

static int Matches(const char *name, const char *ext, const char *pattern) {
    char path[256];
    TmpPath(path, sizeof(path), name, ext);
    char *text = ReadFile(path);
    regex_t re;
    int found = 0;
    if (text && regcomp(&re, pattern, REG_EXTENDED | REG_NEWLINE | REG_NOSUB) == 0) {
        found = regexec(&re, text, 0, NULL, 0) == 0;
        regfree(&re);
    }
    free(text);
    return found;
}

// Проверка кода выхода; заодно ловим отчёты TSan (если тестируется сборка -tsan)
static void CheckCode(const char *name, int code, int expected) {
    char what[64];
    if (code == -2) {
        snprintf(what, sizeof(what), "timed out (expected exit code %d)", expected);
    } else {
        snprintf(what, sizeof(what), "exit code %d (expected %d)", code, expected);
    }
    Report(code == expected, name, what);
    if (Matches(name, "out", "WARNING: ThreadSanitizer")) {
        Report(0, name, "ThreadSanitizer reported a problem");
    }
}

// Запуск с проверкой кода выхода
static void Run(const char *name, int expected, const char *const args[]) {
    CheckCode(name, Wait(Start(name, args)), expected);
}

static void Expect(const char *name, const char *pattern, const char *what) {
    Report(Matches(name, "out", pattern), name, what);
}

static void NotExpect(const char *name, const char *pattern, const char *what) {
    Report(!Matches(name, "out", pattern), name, what);
}

static int SameFiles(const char *a, const char *b) {
    char *x = ReadFile(a);
    char *y = ReadFile(b);
    int same = x && y && strcmp(x, y) == 0;
    free(x);
    free(y);
    return same;
}

// Копия конфига, где строка "failure = ..." заменена на свою
static void WriteWithFailure(const char *src, const char *dst, const char *failure) {
    char *text = ReadFile(src);
    FILE *f = fopen(dst, "w");
    if (text == NULL || f == NULL) {
        free(text);
        if (f) {
            fclose(f);
        }
        return;
    }
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        fprintf(f, "%s\n", strncmp(line, "failure", 7) == 0 ? failure : line);
    }
    fclose(f);
    free(text);
}

static void RemoveTmpDir(void) {
    DIR *dir = opendir(tmpDir);
    if (dir == NULL) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {  // NOLINT(concurrency-mt-unsafe): один поток
        if (entry->d_name[0] != '.') {
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", tmpDir, entry->d_name);
            unlink(path);
        }
    }
    closedir(dir);
    rmdir(tmpDir);
}

//------------------------------------------------------------------------------

static void TestNormalDay(void) {
    puts("== 1. Normal working day");
    Run("normal", 0, (const char *[]){"-c", "data/normal.cfg", "-s", "42", NULL});
    Expect("normal", "ENTRANCE CLOSED", "entrance closes");
    Expect("normal", "is clean!", "cars are washed");
    Expect("normal", "Outcome +: finished", "outcome is reported");
    Expect("normal", "still inside: 0", "every accepted car is washed");
    NotExpect("normal", "soft assert", "no soft asserts");

    char log[256];
    TmpPath(log, sizeof(log), "normal", "log");
    char *journal = ReadFile(log);
    int ok = journal && strstr(journal, "is clean!") && strstr(journal, "RESULTS");
    Report(ok, "normal", "journal has the feed and results");
    free(journal);
}

static void TestAllEvents(void) {
    puts("== 2. All required events are shown");
    Run("events", 0, (const char *[]){"-c", "data/breakdowns.cfg", "-s", "1", NULL});
    const char *patterns[] = {
        "arrives at the entrance",
        "chooses",
        "enters .* post",
        "leaves .* post",
        "waits in .* queue",
        "waits on .* is full",
        "moves .* -> ",
        "starts on car",
        "finished car",
        "BREAKS DOWN",
        "is repaired",
        "is clean",
        "ENTRANCE CLOSED",
    };
    for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); ++i) {
        Expect("events", patterns[i], patterns[i]);
    }
}

static void TestRushHour(void) {
    puts("== 3. Rush hour: blocking and turned-away cars");
    Run("rush", 0, (const char *[]){"-c", "data/rush-hour.cfg", "-s", "3", NULL});
    Expect("rush", "turned away [1-9]", "some cars are turned away");
    Expect("rush", "waits on .* is full", "posts are blocked by a full next stage");
    Expect("rush", "still inside: 0", "every accepted car is washed");
    NotExpect("rush", "soft assert", "no soft asserts");
}

// Есть ли две строки "dry post 1 starts on car" подряд без "leaves dry post 1" между ними
static int TwoCarsOnOnePost(const char *name) {
    char path[256];
    TmpPath(path, sizeof(path), name, "out");
    char *text = ReadFile(path);
    int started = 0, found = 0;
    char *save = NULL;
    for (char *line = text ? strtok_r(text, "\n", &save) : NULL; line && !found;
         line = strtok_r(NULL, "\n", &save)) {
        if (strstr(line, "dry post 1 starts on car")) {
            found = started;
            started = 1;
        } else if (strstr(line, "leaves dry post 1")) {
            started = 0;
        }
    }
    free(text);
    return found;
}

static void TestCapacity(void) {
    puts("== 4. A post of capacity 2 washes two cars at once");
    Run("capacity", 0, (const char *[]){"-c", "data/capacity.cfg", "-s", "7", "-D", NULL});
    Report(TwoCarsOnOnePost("capacity"), "capacity", "two cars start on dry post 1 at once");
    NotExpect("capacity", "soft assert", "no soft asserts");
}

static void TestPriorityAndZeroBuffers(void) {
    puts("== 5. Priority rules and zero-capacity queues");
    Run("priority", 0, (const char *[]){"-c", "data/priority.cfg", "-s", "8", NULL});
    Expect("priority", "Priority rule: program", "rule 'program' is used");
    Expect("priority", "still inside: 0", "every accepted car is washed");
    Run("zero", 0, (const char *[]){"-c", "data/zero-buffer.cfg", "-s", "8", NULL});
    Expect("zero", "no free .* post and no waiting space", "cars go straight to a post");
    Expect("zero", "still inside: 0", "every accepted car is washed");
}

static void TestBadParameters(void) {
    puts("== 6. Bad parameters");
    Run("badroute", 1, (const char *[]){"-c", "data/bad-route.cfg", NULL});
    Expect("badroute", "route must follow the line order", "route order is checked");
    Run("nofile", 1, (const char *[]){"-c", "data/missing.cfg", NULL});
    Expect("nofile", "cannot open config", "missing file is reported");
    Run("badmode", 1, (const char *[]){"-m", "lvie", NULL});
    Expect("badmode", "unknown view mode", "mistyped view mode is rejected");
    Run("badseed", 1, (const char *[]){"-s", "abc", NULL});
    Expect("badseed", "bad seed", "non-numeric seed is rejected");
    Run("badsab", 1, (const char *[]){"-x", "boom", NULL});
    Expect("badsab", "unknown sabotage 'boom'", "unknown sabotage kind is rejected");
    // -D без аргумента: следующий ключ разбирается как ключ, а не как аргумент -D
    Run("dflag", 0, (const char *[]){"-D", "-s", "5", "-c", "data/tiny.cfg", NULL});
    Expect("dflag", "seed: 5", "the option after -D is still parsed");
    Run("extra", 1, (const char *[]){"-D", "extra", NULL});
    Expect("extra", "unexpected argument 'extra'", "a stray argument is rejected");
}

static void TestDeterministicSameSeed(void) {
    puts("== 7. -D: the same seed gives the same journal");
    const char *const args[] = {"-D", "-s", "5", "-c", "data/rush-hour.cfg", NULL};
    CheckCode("det-a", Wait(Start("det-a", args)), 0);
    CheckCode("det-b", Wait(Start("det-b", args)), 0);
    char a[256], b[256];
    TmpPath(a, sizeof(a), "det-a", "log");
    TmpPath(b, sizeof(b), "det-b", "log");
    Report(SameFiles(a, b), "det", "journals are identical byte for byte");
    // без -D совпадения не требуем: порядок потоков решает ОС
    Run("nondet", 0, (const char *[]){"-s", "5", "-c", "data/rush-hour.cfg", NULL});
}

static void TestJournalUnwritable(void) {
    puts("== 8. Journal cannot be opened");
    Run("nojournal", 0,
        (const char *[]){"-c", "data/tiny.cfg", "-l", "/nonexistent/dir/x.log", NULL});
    Expect("nojournal", "cannot open journal", "the problem is reported");
    Expect("nojournal", "is clean!", "the run goes on without the journal");
}

static void TestSemFailure(void) {
    puts("== 9. A semaphore cannot be created");
    // normal.cfg при старте открывает 8 семафоров: часы (1), посты (1+2+1+2=6), координатор
    // (1) - вызовы SemOpen с номерами 0..7. N = 8 + 2 = 10: первые две машины (8 и 9)
    // создаются, а на третьей SemOpen отказывает - ошибка приходит, когда потоки машин уже
    // работают, и программа должна остановить их всех и выйти с кодом 5
    pid_t pid = StartEnv("semfail", "CARWASH_FAIL_SEM_AFTER=10",
                         (const char *[]){"-c", "data/normal.cfg", "-s", "1", NULL});
    CheckCode("semfail", WaitFor(pid, 5), 5);
    Expect("semfail", "car #2 arrives", "cars were already running");
    Expect("semfail", "not enough resources", "the reason is reported");
    // отказ на самом первом семафоре (часы): выход до запуска потоков
    pid = StartEnv("semfail0", "CARWASH_FAIL_SEM_AFTER=0",
                   (const char *[]){"-c", "data/normal.cfg", NULL});
    CheckCode("semfail0", WaitFor(pid, 5), 5);
}

// Сколько операций в выводе длились не столько, сколько объявлено в "starts on car ... (N min)"
// (без поломок операция идёт ровно N минут). В *ops - сколько операций проверено
static int WrongOperationLengths(const char *name, int *ops) {
    char path[256];
    TmpPath(path, sizeof(path), name, "out");
    char *text = ReadFile(path);
    struct {
        char key[48];
        int at, len;
    } run[128];
    int opened = 0, wrong = 0;
    *ops = 0;
    char *save = NULL;
    for (char *line = text ? strtok_r(text, "\n", &save) : NULL; line;
         line = strtok_r(NULL, "\n", &save)) {
        int hh, mm, post, car, len;
        char stage[16], prog[16];
        // строки ленты пишет сама программа, числа в них всегда правильные
        // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion,cert-err34-c)
        if (sscanf(line, "[%d:%d] %15s post %d starts on car #%d %15s (%d min)", &hh, &mm, stage,
                   &post, &car, prog, &len) == 7 &&
            opened < 128) {
            snprintf(run[opened].key, sizeof(run[0].key), "%s %d %d", stage, post, car);
            run[opened].at = (hh * 60) + mm;
            run[opened++].len = len;
        } else if (sscanf(  // NOLINT(bugprone-unchecked-string-to-number-conversion,cert-err34-c)
                       line, "[%d:%d] %15s post %d finished car #%d", &hh, &mm, stage, &post,
                       &car) == 5) {
            char key[48];
            snprintf(key, sizeof(key), "%s %d %d", stage, post, car);
            for (int i = 0; i < opened; ++i) {
                if (strcmp(run[i].key, key) == 0) {
                    wrong += (hh * 60) + mm - run[i].at != run[i].len;
                    ++*ops;
                    run[i] = run[--opened];
                    break;
                }
            }
        }
    }
    free(text);
    return wrong;
}

static void TestOperationLength(void) {
    puts("== 10. An operation lasts exactly its duration (threads in parallel)");
    // машина может сесть на пост в ту же минуту, когда поток поста уже отработал или ещё нет;
    // в обоих случаях операция длится объявленное время. Поломок нет, чтобы не было пауз
    char cfg[256];
    TmpPath(cfg, sizeof(cfg), "nofail", "cfg");
    WriteWithFailure("data/rush-hour.cfg", cfg, "failure = 0 1 1");
    int wrong = 0, ops = 0, bad = 0;
    for (int seed = 1; seed <= 10; ++seed) {
        char s[16];
        snprintf(s, sizeof(s), "%d", seed);
        int code = Wait(Start("oplen", (const char *[]){"-c", cfg, "-s", s, NULL}));
        int n = 0;
        wrong += WrongOperationLengths("oplen", &n);
        ops += n;
        bad += code != 0;
    }
    char what[96];
    snprintf(what, sizeof(what), "10 runs exit 0, %d operations, %d of wrong length", ops, wrong);
    Report(bad == 0 && ops > 0 && wrong == 0, "oplen", what);
}

static void SleepMs(long ms) {
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

// Номер (с 1) первой строки вывода, где встречается text, или 0
static long LineOf(const char *name, const char *text) {
    char path[256];
    TmpPath(path, sizeof(path), name, "out");
    char *all = ReadFile(path);
    long n = 0, found = 0;
    char *save = NULL;
    for (char *line = all ? strtok_r(all, "\n", &save) : NULL; line && !found;
         line = strtok_r(NULL, "\n", &save)) {
        ++n;
        if (strstr(line, text)) {
            found = n;
        }
    }
    free(all);
    return found;
}

static void TestStall(void) {
    puts("== 11. Stall: the wait-for graph names the root cause");
    Run("dead", 2, (const char *[]){"-c", "data/dead-stage.cfg", "-s", "3", NULL});
    Expect("dead", "WARNING: program Premium needs stage wax", "warning at start");
    Expect("dead", "STALL: .* stage wax has no posts", "stall names the stage");
    Expect("dead", "WAITS-FOR GRAPH", "wait-for graph is printed");
    Expect("dead", "ROOT CAUSE: stage wax has no posts", "graph names the root cause");
    Expect("dead", "Outcome +: STALLED", "outcome is reported");
    NotExpect("dead", "soft assert", "no soft asserts");

    Run("dead-D", 2, (const char *[]){"-c", "data/dead-stage.cfg", "-s", "3", "-D", NULL});
    Expect("dead-D", "ROOT CAUSE: stage wax has no posts", "-D: graph names the root cause");

    // одна невыполнимая программа не должна останавливать мойку для остальных
    Run("deadprog", 2, (const char *[]){"-c", "data/dead-program.cfg", "-s", "2", NULL});
    long closed = LineOf("deadprog", "ENTRANCE CLOSED");
    long stall = LineOf("deadprog", "STALL:");
    Report(closed > 0 && stall > closed, "deadprog", "stall comes only after the entrance closes");

    // в ИЗ1 из-за поломок пустых постов тупик находился только на 29-е сутки. При частых
    // поломках машины Premium часто получают отказ все, и день кончается нормально (так при
    // -s 2): берём seed, где Premium попадает на линию, и -D, чтобы исход не зависел от ОС
    char cfg[256];
    TmpPath(cfg, sizeof(cfg), "deadfail", "cfg");
    WriteWithFailure("data/dead-stage.cfg", cfg, "failure = 0.2 5 40");
    Run("deadfail", 2, (const char *[]){"-c", cfg, "-s", "3", "-D", NULL});
    Expect("deadfail", "STALL: .* stage wax has no posts", "the stall is reported");
    NotExpect("deadfail", "^\\[d[0-9]+ ", "stall is found on the first day despite failures");
}

static void TestStallDot(void) {
    puts("== 12. Stall: the wait-for graph in DOT (-g)");
    char dot[256];
    TmpPath(dot, sizeof(dot), "g", "dot");
    Run("deaddot", 2, (const char *[]){"-c", "data/dead-stage.cfg", "-s", "3", "-g", dot, NULL});
    char *text = ReadFile(dot);
    Report(text && strncmp(text, "digraph", 7) == 0, "deaddot", "the file starts with digraph");
    Report(text && strstr(text, "wax"), "deaddot", "the graph mentions stage wax");
    free(text);
}

static void TestSignals(void) {
    puts("== 13. Unlimited mode is stopped by signals");
    const int signals[] = {SIGINT, SIGTERM};
    const char *names[] = {"SIGINT", "SIGTERM"};
    for (int i = 0; i < 2; ++i) {
        pid_t pid = Start(names[i], (const char *[]){"-u", "-d", "5", "-s", "5", NULL});
        SleepMs(300);
        kill(pid, signals[i]);
        CheckCode(names[i], Wait(pid), 3);
        Expect(names[i], "INTERRUPTED by user", "interruption is reported");

        char log[256];
        TmpPath(log, sizeof(log), names[i], "log");
        char *journal = ReadFile(log);
        Report(journal && strstr(journal, "INTERRUPTED") && strstr(journal, "RESULTS"), names[i],
               "journal has the interruption and results");
        free(journal);
    }
}

static void TestSnapshotSignal(void) {
    puts("== 14. SIGUSR1 prints a snapshot and the run goes on");
    pid_t pid = Start("usr1", (const char *[]){"-u", "-d", "5", "-s", "5", NULL});
    SleepMs(300);
    kill(pid, SIGUSR1);
    SleepMs(300);
    kill(pid, SIGINT);
    CheckCode("usr1", Wait(pid), 3);
    Expect("usr1", "WAITS-FOR GRAPH", "wait-for graph is printed");
    Expect("usr1", "^THREADS", "thread table is printed");
    Expect("usr1", "coordinator", "the table lists the coordinator");
}

static void TestCloseSignal(void) {
    puts("== 15. SIGUSR2 closes the entrance early");
    pid_t pid = Start("usr2", (const char *[]){"-u", "-d", "5", "-s", "5", NULL});
    SleepMs(300);
    kill(pid, SIGUSR2);
    CheckCode("usr2", Wait(pid), 0);
    Expect("usr2", "ENTRANCE CLOSED", "entrance closes");
    Expect("usr2", "Outcome +: finished", "accepted cars are washed");
    Expect("usr2", "still inside: 0", "nobody is left inside");
}

// Сумма мест линии (посты x ёмкость + очереди) по строкам ConfigPrint из вывода, или -1
static int LinePlaces(const char *name) {
    char path[256];
    TmpPath(path, sizeof(path), name, "out");
    char *all = ReadFile(path);
    int places = 0, stages = 0;
    char *save = NULL;
    for (char *line = all ? strtok_r(all, "\n", &save) : NULL; line;
         line = strtok_r(NULL, "\n", &save)) {
        char stage[16];
        int posts = 0, cap = 1, queue = 0;
        // строки печатает сама программа: "  tunnel    posts 2 x2, operation 6-9 min, queue 2"
        // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion,cert-err34-c)
        if (sscanf(line, "  %15s posts %d", stage, &posts) != 2) {
            continue;
        }
        const char *x = strstr(line, " x");
        const char *q = strstr(line, "queue ");
        if (x && x < strstr(line, ",")) {
            cap = (int)strtol(x + 2, NULL, 10);
        }
        if (q) {
            queue = (int)strtol(q + 6, NULL, 10);
        }
        places += (posts * cap) + queue;
        ++stages;
    }
    free(all);
    return stages > 0 ? places : -1;
}

static void TestUnlimitedReaping(void) {
    puts("== 16. Unlimited mode: car threads are reaped as they leave");
    pid_t pid = Start("reap", (const char *[]){"-u", "-d", "0", "-s", "3", NULL});
    SleepMs(2000);
    kill(pid, SIGINT);
    CheckCode("reap", Wait(pid), 3);
    char path[256];
    TmpPath(path, sizeof(path), "reap", "out");
    char *all = ReadFile(path);
    const char *at = all ? strstr(all, "\nThreads ") : NULL;
    int created = -1, cars = -1, peak = -1;
    long parks = -1;
    // NOLINTNEXTLINE(bugprone-unchecked-string-to-number-conversion,cert-err34-c)
    int got = at ? sscanf(at, "\nThreads : created %d (cars %d), peak alive cars %d, parks %ld",
                          &created, &cars, &peak, &parks)
                 : 0;
    free(all);
    int places = LinePlaces("reap");
    char what[160];
    snprintf(what, sizeof(what), "created %d threads (%d cars), peak alive cars %d <= %d + 1",
             created, cars, peak, places);
    Report(got == 4 && places > 0 && created > 100 && peak <= places + 1 && peak > 0, "reap", what);
}

static void TestSabotageOverflow(void) {
    puts("== 17. Sabotage -x overflow: the auditor and SOFT_ASSERT catch it");
    Run("overflow", 4,
        (const char *[]){"-x", "overflow", "-c", "data/rush-hour.cfg", "-s", "3", NULL});
    Expect("overflow", "I5 violated", "the auditor reports I5");
    Expect("overflow", "soft assert failed", "SOFT_ASSERT fires too");
    Run("overflow-D", 4,
        (const char *[]){"-x", "overflow", "-c", "data/rush-hour.cfg", "-s", "3", "-D", NULL});
    Expect("overflow-D", "I5 violated", "-D: the auditor reports I5");
    Expect("overflow-D", "soft assert failed", "-D: SOFT_ASSERT fires too");
}

static void TestSabotageDeadlock(void) {
    puts("== 18. Sabotage -x deadlock: LOCKDEP reports it, the program does not hang");
    // фатальный LOCKDEP выходит через _exit(4) при живых потоках - так задумано, и сборка -tsan
    // сочла бы это утечкой потоков; гонки данных TSan при этом по-прежнему ищет
    pid_t pid = StartEnv("deadlock", "TSAN_OPTIONS=report_thread_leaks=0",
                         (const char *[]){"-x", "deadlock", "-c", "data/rush-hour.cfg", NULL});
    CheckCode("deadlock", WaitFor(pid, 5), 4);
    Expect("deadlock", "LOCKDEP:", "lock order violation is reported");
}

// Оканчивается ли строка s на suffix
static int EndsWith(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static void TestSabotageRaceTsan(void) {
    puts("== 19. Sabotage -x race: ThreadSanitizer catches it");
    if (!EndsWith(bin, "-tsan")) {
        puts("  SKIP race: needs the -tsan build (./bin/run_tests ./bin/carwash-mt-tsan)");
        return;
    }
    // здесь отчёт TSan - ожидаемый результат, поэтому не CheckCode (он считает его провалом).
    // Код выхода не проверяем: найдя гонку, TSan сам меняет его (66, а на macOS по умолчанию
    // abort_on_error=1 - SIGABRT при выходе)
    int code =
        Wait(Start("race", (const char *[]){"-x", "race", "-c", "data/rush-hour.cfg", NULL}));
    Report(code != -2, "race", "the run finishes in time");
    Expect("race", "ThreadSanitizer: data race", "the data race is reported");
}

// Сценарии с ожидаемым кодом выхода для TestManySeeds
static const struct {
    const char *cfg;
    int code;
} SEED_SCENARIOS[] = {
    {"data/normal.cfg", 0},   {"data/rush-hour.cfg", 0},  {"data/breakdowns.cfg", 0},
    {"data/capacity.cfg", 0}, {"data/dead-stage.cfg", 2},
};
#define SEED_BATCH 8  // столько прогонов идут одновременно

static void TestManySeeds(void) {
    puts("== 20. Many seeds: no violations, no soft asserts, no LOCKDEP");
    const int scenarios = (int)(sizeof(SEED_SCENARIOS) / sizeof(SEED_SCENARIOS[0]));
    for (int det = 0; det <= 1; ++det) {
        int seeds = det ? 10 : 40, total = scenarios * seeds, bad = 0, runs = 0;
        char firstBad[160] = "";
        for (int from = 0; from < total; from += SEED_BATCH) {
            pid_t pid[SEED_BATCH];
            char name[SEED_BATCH][32];
            int n = 0;
            for (int i = from; i < total && n < SEED_BATCH; ++i, ++n) {
                char seed[16];
                snprintf(seed, sizeof(seed), "%d", (i % seeds) + 1);
                snprintf(name[n], sizeof(name[n]), "seeds-%d", n);
                const char *cfg = SEED_SCENARIOS[i / seeds].cfg;
                pid[n] =
                    det ? Start(name[n],
                                (const char *[]){"-c", cfg, "-s", seed, "-d", "0", "-D", NULL})
                        : Start(name[n], (const char *[]){"-c", cfg, "-s", seed, "-d", "0", NULL});
            }
            for (int k = 0; k < n; ++k) {
                int i = from + k;
                int code = Wait(pid[k]);
                ++runs;
                int wrong = code != SEED_SCENARIOS[i / seeds].code ||
                            Matches(name[k], "out", "AUDITOR: \\[|violated") ||
                            Matches(name[k], "out", "soft assert") ||
                            Matches(name[k], "out", "LOCKDEP") ||
                            Matches(name[k], "out", "WARNING: ThreadSanitizer");
                if (wrong && bad++ == 0) {
                    snprintf(firstBad, sizeof(firstBad), "; first: %s -s %d%s, exit code %d",
                             SEED_SCENARIOS[i / seeds].cfg, (i % seeds) + 1, det ? " -D" : "",
                             code);
                }
            }
        }
        char what[256];
        snprintf(what, sizeof(what), "%s%d runs, %d bad%s", det ? "-D: " : "", runs, bad, firstBad);
        Report(bad == 0 && runs == total, "seeds", what);
    }
}

static void TestResults(void) {
    puts("== 21. Results: statistics, timeline of posts, auditor summary");
    Run("results", 0, (const char *[]){"-c", "data/rush-hour.cfg", "-s", "3", NULL});
    Expect("results", "RESULTS", "results are printed");
    Expect("results", "By program", "per-program statistics");
    Expect("results", "TIMELINE OF POSTS", "timeline of posts (Gantt chart)");
    Expect("results", "^Threads +: created", "thread statistics");
    Expect("results", "^Blocked time +: [0-9]+ car-minutes waiting for room", "blocked time");
    Expect("results", "Bottleneck", "the bottleneck stage is named");
    Expect("results", "AUDITOR: invariants checked after each of [1-9][0-9]* events, violations: 0",
           "auditor summary");
    char log[256];
    TmpPath(log, sizeof(log), "results", "log");
    char *journal = ReadFile(log);
    Report(journal && strstr(journal, "TIMELINE OF POSTS") && strstr(journal, "AUDITOR"), "results",
           "journal has the timeline and the auditor summary");
    free(journal);
}

// Последняя непустая строка вывода содержит needle
static int LastLineHas(const char *name, const char *needle) {
    char path[256];
    TmpPath(path, sizeof(path), name, "out");
    char *text = ReadFile(path);
    int ok = 0;
    if (text) {
        size_t n = strlen(text);
        while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == ' ')) {
            text[--n] = '\0';
        }
        const char *last = strrchr(text, '\n');
        ok = strstr(last ? last + 1 : text, needle) != NULL;
    }
    free(text);
    return ok;
}

static void TestLiveToFile(void) {
    puts("== 17. Live view into a file");
    Run("live", 0, (const char *[]){"-m", "live", "-d", "0", "-s", "1", NULL});
    Expect("live", "THREADS", "the thread panel is drawn");
    Expect("live", "\033\\[", "ANSI sequences are present");
    Expect("live", "\033\\[\\?25l", "the cursor is hidden while drawing");
    Expect("live", "\033\\[\\?25h", "the cursor is restored");
    NotExpect("live", "^\\[[0-9]{2}:[0-9]{2}\\] car #", "the event feed does not go to the screen");
    Report(LastLineHas("live", "Journal saved"), "live", "the last line is the summary");
    char log[256];
    TmpPath(log, sizeof(log), "live", "log");
    char *journal = ReadFile(log);
    Report(journal && strstr(journal, "arrives at the entrance"), "live",
           "the feed still goes to the journal");
    free(journal);
}

static void TestLiveInterrupt(void) {
    puts("== 18. Live view: interrupt gives the cursor back");
    pid_t pid = Start("liveint", (const char *[]){"-m", "live", "-d", "500", "-u", NULL});
    SleepMs(700);
    kill(pid, SIGINT);
    CheckCode("liveint", WaitFor(pid, 1), 3);
    char path[256];
    TmpPath(path, sizeof(path), "liveint", "out");
    char *text = ReadFile(path);
    // курсор возвращается после последнего кадра (кадр начинается с ESC [ H)
    const char *lastFrame = text ? strstr(text, "\033[H") : NULL;
    for (const char *p = lastFrame; p; p = strstr(p + 1, "\033[H")) {
        lastFrame = p;
    }
    Report(lastFrame && strstr(lastFrame, "\033[?25h"), "liveint",
           "the cursor is shown again after the last frame");
    free(text);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        bin = argv[1];
    }
    if (access(bin, X_OK) != 0) {
        fprintf(stderr, "no %s, build the project first\n", bin);
        return 1;
    }
    snprintf(tmpDir, sizeof(tmpDir), "/tmp/carwash-mt-tests-%d", (int)getpid());
    if (mkdir(tmpDir, 0700) != 0) {
        perror(tmpDir);
        return 1;
    }
    color = isatty(1);
    setvbuf(stdout, NULL, _IONBF, 0);  // чтобы порядок строк не путался с дочерними процессами

    TestNormalDay();
    TestAllEvents();
    TestRushHour();
    TestCapacity();
    TestPriorityAndZeroBuffers();
    TestBadParameters();
    TestDeterministicSameSeed();
    TestJournalUnwritable();
    TestSemFailure();
    TestOperationLength();
    TestStall();
    TestStallDot();
    TestSignals();
    TestSnapshotSignal();
    TestCloseSignal();
    TestUnlimitedReaping();
    TestSabotageOverflow();
    TestSabotageDeadlock();
    TestSabotageRaceTsan();
    TestManySeeds();
    TestResults();
    TestLiveToFile();
    TestLiveInterrupt();

    RemoveTmpDir();
    printf("\nPassed: %d, failed: %d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
