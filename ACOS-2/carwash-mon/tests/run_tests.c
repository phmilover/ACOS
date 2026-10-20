// Сценарные тесты: запускаем bin/carwash-mon на файлах из data/ и проверяем
// код выхода и вывод. Перенос из carwash-mt (ACOS-2/carwash-mt/tests/run_tests.c). Запуск из
// корня проекта:
//   cmake --build build --target check
// или вручную: ./bin/run_tests ./bin/carwash-mon
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

static const char *bin = "./bin/carwash-mon";
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

// Запускает carwash-mon -m log -l <tmp>/<name>.log <args...>, вывод идёт в <tmp>/<name>.out.
// env - "ИМЯ=значение" для дочернего процесса или NULL; in - stdin дочернего процесса или -1
// (как у тестировщика); outFd - stdout или -1 (тот же файл .out, что и stderr); closeFd -
// дескриптор, который дочерний процесс закрывает (или -1). Возвращает pid; дождаться - Wait()
static pid_t StartFull(const char *name, const char *env, int in, int outFd, int closeFd,
                       const char *const args[]) {
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
        dup2(outFd >= 0 ? outFd : fd, 1);
        dup2(fd, 2);
        close(fd);
        if (in >= 0) {
            dup2(in, 0);
        }
        if (closeFd >= 0) {
            close(closeFd);
        }
        if (env) {
            putenv((char *)env);  // NOLINT(concurrency-mt-unsafe): дочерний процесс, один поток
        }
        execv(bin, (char *const *)argv);
        perror("execv");
        _exit(127);
    }
    return pid;
}

static pid_t StartEnv(const char *name, const char *env, const char *const args[]) {
    return StartFull(name, env, -1, -1, -1, args);
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

static void TestThreadFailure(void) {
    puts("== 9. A thread cannot be created");
    // отказ на самом первом потоке (пост): участник барьера старта не появился, барьер не
    // отпустить - программа сразу выходит с кодом 5
    pid_t pid = StartEnv("thrfail0", "CARWASH_FAIL_THREAD_AFTER=0",
                         (const char *[]){"-c", "data/normal.cfg", NULL});
    CheckCode("thrfail0", WaitFor(pid, 5), 5);
    Expect("thrfail0", "cannot create a thread", "the reason is reported");
    // normal.cfg при старте создаёт 8 потоков: посты (1+2+1+2=6), координатор, часы - вызовы
    // ThreadCreate с номерами 0..7. N = 12: машины 8..11 создаются, а на пятой машине поток не
    // создаётся - ошибка приходит, когда потоки машин уже работают, и программа должна
    // остановить их всех, напечатать итоги и выйти с кодом 5
    pid = StartEnv("thrfail", "CARWASH_FAIL_THREAD_AFTER=12",
                   (const char *[]){"-c", "data/normal.cfg", "-s", "1", NULL});
    CheckCode("thrfail", WaitFor(pid, 5), 5);
    Expect("thrfail", "car #4 arrives", "cars were already running");
    Expect("thrfail", "cannot create a thread", "the reason is reported");
    Expect("thrfail", "RESULTS", "the results are printed");
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

static void TestThreadColumn(void) {
    puts("== 11. -t: the thread that reported each event");
    Run("column", 0, (const char *[]){"-t", "-c", "data/normal.cfg", "-s", "3", NULL});
    Expect("column", "^\\[[0-9:]+\\] post tunnel\\.[12] +\\| tunnel post",
           "posts report their work");
    Expect("column", "^\\[[0-9:]+\\] car #[0-9]+ +\\| car #[0-9]+ arrives",
           "cars report themselves");
    Expect("column", "^\\[[0-9:]+\\] coordinator +\\| ENTRANCE CLOSED",
           "the coordinator closes the entrance");
}

// Файл с ответами на вопросы -i. @return открытый на чтение дескриптор или -1
static int AnswersFile(const char *name, const char *text) {
    char path[256];
    TmpPath(path, sizeof(path), name, "in");
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1 || write(fd, text, strlen(text)) < 0) {
        if (fd != -1) {
            close(fd);
        }
        return -1;
    }
    close(fd);
    return open(path, O_RDONLY);
}

static void TestInteractive(void) {
    puts("== 12. -i: parameters from the keyboard");
    int in = AnswersFile("ask", "5\n\n\n0\n\n");  // 5 машин, без поломок, остальное как было
    CheckCode("ask",
              Wait(StartFull("ask", NULL, in, -1, -1,
                             (const char *[]){"-i", "-c", "data/normal.cfg", "-s", "2", NULL})),
              0);
    if (in != -1) {
        close(in);
    }
    Expect("ask", "Number of cars", "the questions are asked");
    Expect("ask", "^Cars: 5,", "the answer is used");
    Expect("ask", "Outcome +: finished", "the day runs to the end");

    // Ctrl+C на вопросе: сигналы ещё не заблокированы, программа прерывается сразу
    int fds[2];
    if (pipe(fds) != 0) {
        Report(0, "ask-int", "pipe");
        return;
    }
    pid_t pid = StartFull("ask-int", NULL, fds[0], -1, fds[1], (const char *[]){"-i", NULL});
    close(fds[0]);
    SleepMs(300);
    kill(pid, SIGINT);
    int code = WaitFor(pid, 2);
    close(fds[1]);
    Report(code == -1, "ask-int", "SIGINT at a question stops the program at once");
}

static void TestClosedPipe(void) {
    puts("== 13. stdout is a closed pipe (carwash-mon -m log | head)");
    int fds[2];
    if (pipe(fds) != 0) {
        Report(0, "epipe", "pipe");
        return;
    }
    close(fds[0]);  // читателя нет: первая же запись даёт SIGPIPE/EPIPE
    pid_t pid = StartFull("epipe", NULL, -1, fds[1], -1,
                          (const char *[]){"-c", "data/tiny.cfg", "-s", "1", NULL});
    close(fds[1]);
    CheckCode("epipe", Wait(pid), 0);
    char log[256];
    TmpPath(log, sizeof(log), "epipe", "log");
    char *journal = ReadFile(log);
    Report(journal && strstr(journal, "RESULTS"), "epipe", "the journal still has the results");
    free(journal);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        bin = argv[1];
    }
    if (access(bin, X_OK) != 0) {
        fprintf(stderr, "no %s, build the project first\n", bin);
        return 1;
    }
    snprintf(tmpDir, sizeof(tmpDir), "/tmp/carwash-mon-tests-%d", (int)getpid());
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
    TestThreadFailure();
    TestOperationLength();
    TestThreadColumn();
    TestInteractive();
    TestClosedPipe();

    RemoveTmpDir();
    printf("\nPassed: %d, failed: %d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
