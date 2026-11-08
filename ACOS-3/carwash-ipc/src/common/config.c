#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"

#define MAX_TOKENS 16
#define FILE_LIMIT 16384
#define MAX_WEIGHT 1000000  // чтобы сумма весов точно влезла в int

// Обычная городская мойка. Разбирается так же, как файл из -c.
const char *CONFIG_BUILTIN = "stage   = prewash 1 3 5 3\n"
                             "stage   = tunnel  2 6 9 2\n"
                             "stage   = wax     1 4 6 2\n"
                             "stage   = dry     2 3 5 3\n"
                             "program = Express  40 1 tunnel dry\n"
                             "program = Standard 40 2 prewash tunnel dry\n"
                             "program = Premium  20 3 prewash tunnel wax dry\n"
                             "cars     = 40\n"
                             "arrival  = 2 6\n"
                             "day      = 240\n"
                             "failure  = 0.05 5 15\n"
                             "priority = fifo\n";

void ConfigDefaults(Config *c) {
    memset(c, 0, sizeof(*c));
    c->cars = 40;
    c->arrMin = 2;
    c->arrMax = 6;
    c->day = 240;
    c->failP = 0.05;
    c->repairMin = 5;
    c->repairMax = 15;
    c->rule = PRIO_FIFO;
    c->delayMs = 100;
    c->seed = (unsigned)time(NULL);  // повторить прогон можно, указав этот seed в -s
    snprintf(c->journal, sizeof(c->journal), "carwash.log");
}

const char *ConfigRuleName(PriorityRule rule) {
    static const char *names[] = {"fifo", "program", "shortest"};
    return names[rule];
}

/// strtol с проверкой, что строка целиком - число и влезает в int
int ParseInt(const char *s, int *out) {
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || errno == ERANGE || v < INT_MIN || v > INT_MAX) {
        return -1;
    }
    *out = (int)v;
    return 0;
}

static int ToDouble(const char *s, double *out) {
    char *end;
    *out = strtod(s, &end);
    return (*s == '\0' || *end != '\0') ? -1 : 0;
}

static int StageIndex(const Config *c, const char *name) {
    for (int i = 0; i < c->stages; ++i) {
        if (strcmp(c->stage[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int ParseRule(const char *s, PriorityRule *out) {
    for (int r = PRIO_FIFO; r <= PRIO_SHORTEST; ++r) {
        if (strcmp(s, ConfigRuleName((PriorityRule)r)) == 0) {
            *out = (PriorityRule)r;
            return 0;
        }
    }
    return -1;
}

/// key = tok[0] tok[1] ... tok[n-1]
static int ParseKey(Config *c, const char *key, char **tok, int n, char *err, int errSize) {
    int bad = 0;  // не 0 - значение не разобралось
    if (strcmp(key, "stage") == 0 && n == 5) {
        if (c->stages == MAX_STAGES) {
            snprintf(err, errSize, "too many stages (max %d)", MAX_STAGES);
            return -1;
        }
        StageSpec *s = &c->stage[c->stages++];
        snprintf(s->name, NAME_LEN, "%s", tok[0]);
        bad = ParseInt(tok[1], &s->posts) || ParseInt(tok[2], &s->tmin) ||
              ParseInt(tok[3], &s->tmax) || ParseInt(tok[4], &s->queue);
    } else if (strcmp(key, "program") == 0 && n >= 4) {
        if (c->programs == MAX_PROGRAMS || n - 3 > MAX_STAGES) {
            snprintf(err, errSize, "too many programs or stages in a route");
            return -1;
        }
        ProgramSpec *p = &c->program[c->programs++];
        snprintf(p->name, NAME_LEN, "%s", tok[0]);
        bad = ParseInt(tok[1], &p->weight) || ParseInt(tok[2], &p->priority);
        p->len = 0;
        for (int i = 3; i < n; ++i) {
            int s = StageIndex(c, tok[i]);
            if (s < 0) {
                snprintf(err, errSize, "program %s: unknown stage '%s'", p->name, tok[i]);
                return -1;
            }
            p->route[p->len++] = s;
        }
    } else if (strcmp(key, "cars") == 0 && n == 1) {
        bad = ParseInt(tok[0], &c->cars);
    } else if (strcmp(key, "arrival") == 0 && n == 2) {
        bad = ParseInt(tok[0], &c->arrMin) || ParseInt(tok[1], &c->arrMax);
    } else if (strcmp(key, "day") == 0 && n == 1) {
        bad = ParseInt(tok[0], &c->day);
    } else if (strcmp(key, "failure") == 0 && n == 3) {
        bad = ToDouble(tok[0], &c->failP) || ParseInt(tok[1], &c->repairMin) ||
              ParseInt(tok[2], &c->repairMax);
    } else if (strcmp(key, "priority") == 0 && n == 1) {
        bad = ParseRule(tok[0], &c->rule);
    } else if (strcmp(key, "seed") == 0 && n == 1) {
        int seed = 0;
        bad = ParseInt(tok[0], &seed);
        c->seed = (unsigned)seed;
    } else if (strcmp(key, "delay") == 0 && n == 1) {
        bad = ParseInt(tok[0], &c->delayMs);
    } else {
        snprintf(err, errSize, "unknown key '%s' or wrong number of values", key);
        return -1;
    }
    if (bad) {
        snprintf(err, errSize, "bad value for '%s'", key);
        return -1;
    }
    return 0;
}

int ConfigParseText(Config *c, const char *text, char *err, int errSize) {
    int lineNo = 0;
    while (*text) {
        char line[512];
        size_t len = strcspn(text, "\n");
        if (len >= sizeof(line)) {
            len = sizeof(line) - 1;
        }
        memcpy(line, text, len);
        line[len] = '\0';
        text += strcspn(text, "\n");
        if (*text == '\n') {
            ++text;
        }
        ++lineNo;

        char *hash = strchr(line, '#');
        if (hash) {
            *hash = '\0';
        }
        char *eq = strchr(line, '=');
        char *save = NULL;
        char *key = strtok_r(line, " \t\r=", &save);
        if (key == NULL) {
            continue;  // пустая строка или только комментарий
        }
        if (eq == NULL) {
            snprintf(err, errSize, "line %d: expected 'key = value'", lineNo);
            return -1;
        }
        char *tok[MAX_TOKENS];
        int n = 0;
        char *save2 = NULL;
        for (char *t = strtok_r(eq + 1, " \t\r,", &save2); t && n < MAX_TOKENS;
             t = strtok_r(NULL, " \t\r,", &save2)) {
            tok[n++] = t;
        }
        char msg[200];
        if (ParseKey(c, key, tok, n, msg, sizeof(msg)) != 0) {
            snprintf(err, errSize, "line %d: %s", lineNo, msg);
            return -1;
        }
    }
    return 0;
}

int ConfigLoadFile(Config *c, const char *path, char *err, int errSize) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) {
        snprintf(err, errSize, "cannot open config '%s': %s", path, strerror(errno));
        return -1;
    }
    // read() может вернуть меньше, чем просили, поэтому читаем в цикле
    static char buf[FILE_LIMIT + 1];
    size_t total = 0;
    ssize_t got;
    while (total < FILE_LIMIT && (got = read(fd, buf + total, FILE_LIMIT - total)) > 0) {
        total += (size_t)got;
    }
    close(fd);
    buf[total] = '\0';
    return ConfigParseText(c, buf, err, errSize);
}

int ConfigValidate(const Config *c, char *err, int errSize) {
    if (c->stages == 0 || c->programs == 0) {
        snprintf(err, errSize, "at least one stage and one program are required");
        return -1;
    }
    for (int i = 0; i < c->stages; ++i) {
        const StageSpec *s = &c->stage[i];
        if (s->posts < 0 || s->posts > MAX_POSTS || s->tmin < 1 || s->tmax < s->tmin ||
            s->queue < 0 || s->queue >= MAX_QUEUE) {
            snprintf(err, errSize, "stage %s: need 0<=posts<=%d, 1<=tmin<=tmax, 0<=queue<%d",
                     s->name, MAX_POSTS, MAX_QUEUE);
            return -1;
        }
    }
    for (int i = 0; i < c->programs; ++i) {
        const ProgramSpec *p = &c->program[i];
        if (p->weight < 1 || p->weight > MAX_WEIGHT) {
            snprintf(err, errSize, "program %s: weight must be 1..%d", p->name, MAX_WEIGHT);
            return -1;
        }
        // линия едет только вперёд, поэтому ожидание не может замкнуться в цикл
        for (int k = 1; k < p->len; ++k) {
            if (p->route[k] <= p->route[k - 1]) {
                snprintf(err, errSize, "program %s: route must follow the line order", p->name);
                return -1;
            }
        }
    }
    if (c->arrMin < 1 || c->arrMax < c->arrMin || c->cars < 0 || c->day < 0) {
        snprintf(err, errSize, "need 1<=arrival min<=max, cars>=0, day>=0");
        return -1;
    }
    if (c->failP < 0 || c->failP > 1 || (c->failP > 0 && c->repairMin < 1) ||
        c->repairMax < c->repairMin) {
        snprintf(err, errSize, "need 0<=failure p<=1 and 1<=repair min<=max");
        return -1;
    }
    if (c->delayMs < 1 || c->delayMs > 10000) {
        snprintf(err, errSize, "delay must be 1..10000 ms per model minute");
        return -1;
    }
    return 0;
}

int ConfigDeadStage(const Config *c, int program) {
    const ProgramSpec *p = &c->program[program];
    for (int k = 0; k < p->len; ++k) {
        if (c->stage[p->route[k]].posts == 0) {
            return p->route[k];
        }
    }
    return -1;
}

int ConfigPossiblePrograms(const Config *c) {
    int n = 0;
    for (int i = 0; i < c->programs; ++i) {
        n += ConfigDeadStage(c, i) < 0;
    }
    return n;
}

int ConfigTotalPosts(const Config *c) {
    int n = 0;
    for (int i = 0; i < c->stages; ++i) {
        n += c->stage[i].posts;
    }
    return n;
}

void ConfigPrint(const Config *c, int fd) {
    dprintf(fd, "Stages:\n");
    for (int i = 0; i < c->stages; ++i) {
        const StageSpec *s = &c->stage[i];
        dprintf(fd, "  %-9s posts %d, operation %d-%d min, queue %d\n", s->name, s->posts, s->tmin,
                s->tmax, s->queue);
    }
    dprintf(fd, "Programs:\n");
    for (int i = 0; i < c->programs; ++i) {
        const ProgramSpec *p = &c->program[i];
        dprintf(fd, "  %-9s weight %d, priority %d, route:", p->name, p->weight, p->priority);
        for (int k = 0; k < p->len; ++k) {
            dprintf(fd, "%s%s", k ? " > " : " ", c->stage[p->route[k]].name);
        }
        int dead = ConfigDeadStage(c, i);
        if (dead >= 0) {
            dprintf(fd, "  (impossible: %s has no posts)", c->stage[dead].name);
        }
        dprintf(fd, "\n");
    }
    if (c->cars) {
        dprintf(fd, "Cars: %d", c->cars);
    } else {
        dprintf(fd, "Cars: unlimited");
    }
    dprintf(fd, ", arrival every %d-%d min, working day: ", c->arrMin, c->arrMax);
    if (c->day) {
        dprintf(fd, "%d min\n", c->day);
    } else {
        dprintf(fd, "unlimited (stop with Ctrl+C or SIGUSR2)\n");
    }
    dprintf(fd, "Failures: p=%.3f after each operation, repair %d-%d min\n", c->failP, c->repairMin,
            c->repairMax);
    dprintf(fd, "Priority rule: %s, seed: %u, %d ms per model minute\n", ConfigRuleName(c->rule),
            c->seed, c->delayMs);
}

/// Ищет в argv значение ключа -c ("-c FILE" или "-cFILE"); NULL - ключа нет.
static const char *FindConfigPath(int argc, char **argv) {
    const char *path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--") == 0) {
            break;
        }
        if (strncmp(argv[i], "-c", 2) == 0) {
            path = argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[i + 1] : NULL);
        }
    }
    return path;
}

int ConfigFromArgs(Config *c, int argc, char **argv, const char *extra,
                   int (*onExtra)(int opt, const char *arg)) {
    char err[300];
    ConfigDefaults(c);
    const char *path = FindConfigPath(argc, argv);
    int rc = path ? ConfigLoadFile(c, path, err, sizeof(err))
                  : ConfigParseText(c, CONFIG_BUILTIN, err, sizeof(err));
    if (rc != 0) {
        dprintf(2, "%s: %s\n", argv[0], err);
        return -1;
    }

    char opts[64];
    snprintf(opts, sizeof(opts), "c:s:d:l:uq%s", extra);
    int opt;
    while ((opt = getopt(argc, argv, opts)) != -1) {
        int bad = 0;
        switch (opt) {
        case 'c': break;  // файл уже прочитан
        case 's': {
            int seed;
            bad = ParseInt(optarg, &seed);
            c->seed = (unsigned)seed;
            break;
        }
        case 'd': bad = ParseInt(optarg, &c->delayMs); break;
        case 'l': snprintf(c->journal, sizeof(c->journal), "%s", optarg); break;
        case 'u':
            c->cars = 0;
            c->day = 0;
            break;
        case 'q': c->quiet = 1; break;
        case '?': return -1;
        default: bad = onExtra ? onExtra(opt, optarg) : -1;
        }
        if (bad) {
            dprintf(2, "%s: bad value for -%c\n", argv[0], opt);
            return -1;
        }
    }
    if (ConfigValidate(c, err, sizeof(err)) != 0) {
        dprintf(2, "%s: %s\n", argv[0], err);
        return -1;
    }
    return 0;
}
