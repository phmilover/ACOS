#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "soft_assert.h"

static long failures = 0;

int SoftAssertFail(const char *cond, const char *msg, const char *file, int line,
                   const char *func) {
    ++failures;
    const char *slash = strrchr(file, '/');  // CMake передаёт полный путь, хватит имени
    if (slash) {
        file = slash + 1;
    }
    dprintf(2, "%scw: soft assert failed: %s (%s), %s:%d in %s()%s\n",
            isatty(2) ? "\033[1;33m" : "", cond, msg, file, line, func, isatty(2) ? "\033[0m" : "");
    return 0;
}

long SoftAssertFailures(void) {
    return failures;
}
