#include "view_log.h"

#include <stdio.h>
#include <unistd.h>

#define SOURCE_WIDTH 14

static int screen = -1;
static int useColor = 0;
static int journal = -1;
static int column = 0;

void ViewLogInit(int screenFd, int color, int journalFd, int threadColumn) {
    screen = screenFd;
    useColor = color;
    journal = journalFd;
    column = threadColumn;
}

/// Одним write, чтобы строка не разорвалась. Писать некуда - молча пропускаем.
static void WriteLine(int fd, const char *line, int n) {
    if (n > 0 && write(fd, line, (size_t)n) < 0) {
        // экран или журнал закрыты, ленте падать незачем
    }
}

void ViewLogListener(const Event *e) {
    char text[200], clock[16], src[40], prefix[64] = "";
    if (EventDescribe(e, text, sizeof(text)) == 0) {
        return;
    }
    EventClock(e->time, clock, sizeof(clock));
    if (column) {
        EventSource(e, src, sizeof(src));
        snprintf(prefix, sizeof(prefix), "%-*s | ", SOURCE_WIDTH, src);
    }
    char line[400];
    if (screen >= 0) {
        int n = useColor ? snprintf(line, sizeof(line), "\033[2m[%s]\033[0m %s%s%s\033[0m\n", clock,
                                    EventColor(e->type), prefix, text)
                         : snprintf(line, sizeof(line), "[%s] %s%s\n", clock, prefix, text);
        WriteLine(screen, line, n < (int)sizeof(line) ? n : (int)sizeof(line) - 1);
    }
    if (journal >= 0) {
        int n = snprintf(line, sizeof(line), "[%s] %s%s\n", clock, prefix, text);
        WriteLine(journal, line, n < (int)sizeof(line) ? n : (int)sizeof(line) - 1);
    }
}
