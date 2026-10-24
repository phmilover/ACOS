#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"

#define MAX_TOKENS 16
#define FILE_LIMIT 16384
#define MAX_WEIGHT 1000000  // чтобы сумма весов точно влезла в int

// Обычная городская мойка. Парсится так же, как файл из -c.
const char *CONFIG_BUILTIN = "# stage   = name posts tmin tmax queue [capacity]\n"
                             "stage   = prewash 1 3 5 3\n"
                             "stage   = tunnel  2 6 9 2\n"
                             "stage   = wax     1 4 6 2\n"
                             "stage   = dry     2 3 5 3\n"
                             "# program = name weight priority stages...\n"
                             "program = Express  40 1 tunnel dry\n"
                             "program = Standard 40 2 prewash tunnel dry\n"
                             "program = Premium  20 3 prewash tunnel wax dry\n"
                             "cars     = 40\n"
                             "arrival  = 2 6\n"
                             "day      = 240\n"
                             "failure  = 0.01 5 15\n"
                             "priority = fifo\n";

void ConfigDefaults(Config *c) {
    memset(c, 0, sizeof(*c));
    c->cars = 40;
    c->arrMin = 2;
    c->arrMax = 6;
    c->day = 240;
    c->failP = 0.01;
    c->repairMin = 5;
    c->repairMax = 15;
    c->rule = PRIO_FIFO;
    c->delayMs = -1;
    c->view = VIEW_AUTO;
    strcpy(c->journal, "carwash.log");
}

int ConfigParseSabotage(const char *s, Sabotage *out) {
    static const struct {
        const char *name;
        Sabotage kind;
    } kinds[] = {{"overflow", SAB_OVERFLOW}, {"race", SAB_RACE}, {"deadlock", SAB_DEADLOCK}};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
        if (strcmp(s, kinds[i].name) == 0) {
            *out = kinds[i].kind;
            return 0;
        }
    }
    return -1;
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

/// "2 6" -> 2 и 6
static int ParsePair(char *s, int *a, int *b) {
    char *save = NULL;
    char *first = strtok_r(s, " \t", &save);
    char *second = strtok_r(NULL, " \t", &save);
    if (first == NULL || second == NULL) {
        return -1;
    }
    return (ParseInt(first, a) || ParseInt(second, b)) ? -1 : 0;
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

/// key = tok[0] tok[1] ... tok[n-1]
static int ParseKey(Config *c, const char *key, char **tok, int n, char *err, int errSize) {
    int bad = 0;  // не 0 - значение не разобралось
    if (strcmp(key, "stage") == 0 && (n == 5 || n == 6)) {
        if (c->stages == MAX_STAGES) {
            snprintf(err, errSize, "too many stages (max %d)", MAX_STAGES);
            return -1;
        }
        StageSpec *s = &c->stage[c->stages++];
        snprintf(s->name, NAME_LEN, "%s", tok[0]);
        bad = ParseInt(tok[1], &s->posts) || ParseInt(tok[2], &s->tmin) ||
              ParseInt(tok[3], &s->tmax) || ParseInt(tok[4], &s->queue);
        s->capacity = 1;  // шестое поле необязательно
        if (n == 6) {
            bad = bad || ParseInt(tok[5], &s->capacity);
        }
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
        if (strcmp(tok[0], "fifo") == 0) {
            c->rule = PRIO_FIFO;
        } else if (strcmp(tok[0], "program") == 0) {
            c->rule = PRIO_PROGRAM;
        } else if (strcmp(tok[0], "shortest") == 0) {
            c->rule = PRIO_SHORTEST;
        } else {
            bad = 1;
        }
    } else if (strcmp(key, "seed") == 0 && n == 1) {
        int seed = 0;
        bad = ParseInt(tok[0], &seed);
        c->seed = (unsigned)seed;
        c->seedSet = 1;
    } else if (strcmp(key, "delay") == 0 && n == 1) {
        bad = ParseInt(tok[0], &c->delayMs);
    } else if (strcmp(key, "view") == 0 && n == 1) {
        if (strcmp(tok[0], "log") == 0) {
            c->view = VIEW_LOG;
        } else if (strcmp(tok[0], "live") == 0) {
            c->view = VIEW_LIVE;
        } else {
            bad = 1;
        }
    } else if (strcmp(key, "journal") == 0 && n == 1) {
        snprintf(c->journal, sizeof(c->journal), "%s", tok[0]);
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
        snprintf(err, errSize, "cannot open config '%s'", path);
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
            s->queue < 0 || s->queue >= MAX_QUEUE || s->capacity < 1 ||
            s->capacity > MAX_CAPACITY) {
            snprintf(err, errSize,
                     "stage %s: need 0<=posts<=%d, 1<=tmin<=tmax, 0<=queue<%d, 1<=capacity<=%d",
                     s->name, MAX_POSTS, MAX_QUEUE, MAX_CAPACITY);
            return -1;
        }
    }
    for (int i = 0; i < c->programs; ++i) {
        const ProgramSpec *p = &c->program[i];
        if (p->weight < 1 || p->weight > MAX_WEIGHT) {
            snprintf(err, errSize, "program %s: weight must be 1..%d", p->name, MAX_WEIGHT);
            return -1;
        }
        // линия едет только вперёд, поэтому маршрут должен идти по возрастанию стадий
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
    return 0;
}

static int AskLine(const char *question, const char *current, char *buf, int size) {
    dprintf(1, "  %-38s [%s]: ", question, current);
    int n = 0;
    char ch;
    while (read(0, &ch, 1) == 1 && ch != '\n') {
        if (n < size - 1) {
            buf[n++] = ch;
        }
    }
    buf[n] = '\0';
    return n;  // 0 - пользователь нажал Enter, оставляем как было
}

void ConfigInteractive(Config *c) {
    char buf[64], cur[64];
    int v, v2;
    double d;
    dprintf(1, "Interactive setup (press Enter to keep the value in brackets)\n");

    snprintf(cur, sizeof(cur), "%d", c->cars);
    if (AskLine("Number of cars (0 = unlimited)", cur, buf, sizeof(buf)) &&
        ParseInt(buf, &v) == 0) {
        c->cars = v;
    }
    snprintf(cur, sizeof(cur), "%d", c->day);
    if (AskLine("Working day, minutes (0 = unlimited)", cur, buf, sizeof(buf)) &&
        ParseInt(buf, &v) == 0) {
        c->day = v;
    }
    snprintf(cur, sizeof(cur), "%d %d", c->arrMin, c->arrMax);
    if (AskLine("Arrival interval 'min max', minutes", cur, buf, sizeof(buf)) &&
        ParsePair(buf, &v, &v2) == 0) {
        c->arrMin = v;
        c->arrMax = v2;
    }
    snprintf(cur, sizeof(cur), "%.3f", c->failP);
    if (AskLine("Failure probability per minute", cur, buf, sizeof(buf)) &&
        ToDouble(buf, &d) == 0) {
        c->failP = d;
    }
    snprintf(cur, sizeof(cur), "%s", ConfigRuleName(c->rule));
    if (AskLine("Priority rule (fifo/program/shortest)", cur, buf, sizeof(buf))) {
        char err[100];
        char *tok[1] = {buf};
        ParseKey(c, "priority", tok, 1, err, sizeof(err));
    }
}

void ConfigPrint(const Config *c, int fd) {
    dprintf(fd, "Stages:\n");
    for (int i = 0; i < c->stages; ++i) {
        const StageSpec *s = &c->stage[i];
        char cap[8] = "";
        if (s->capacity != 1) {
            snprintf(cap, sizeof(cap), " x%d", s->capacity);  // машин на одном посту
        }
        dprintf(fd, "  %-9s posts %d%s, operation %d-%d min, queue %d\n", s->name, s->posts, cap,
                s->tmin, s->tmax, s->queue);
    }
    dprintf(fd, "Programs:\n");
    for (int i = 0; i < c->programs; ++i) {
        const ProgramSpec *p = &c->program[i];
        dprintf(fd, "  %-9s weight %d, priority %d, route:", p->name, p->weight, p->priority);
        for (int k = 0; k < p->len; ++k) {
            dprintf(fd, "%s%s", k ? " > " : " ", c->stage[p->route[k]].name);
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
        dprintf(fd, "unlimited (stop with Ctrl+C)\n");
    }
    dprintf(fd, "Failures: p=%.3f per post per minute, repair %d-%d min\n", c->failP, c->repairMin,
            c->repairMax);
    dprintf(fd, "Priority rule: %s, seed: %u\n", ConfigRuleName(c->rule), c->seed);

    // программа через стадию без постов никогда не закончится - предупредим заранее
    for (int i = 0; i < c->programs; ++i) {
        const ProgramSpec *p = &c->program[i];
        for (int k = 0; k < p->len; ++k) {
            if (c->stage[p->route[k]].posts == 0) {
                dprintf(fd, "WARNING: program %s needs stage %s which has no posts\n", p->name,
                        c->stage[p->route[k]].name);
            }
        }
    }
}
