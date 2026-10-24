#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "soft_assert.h"

static _Atomic long failures = 0;

int SoftAssertFail(const char *cond, const char *msg, const char *file, int line,
                   const char *func) {
    atomic_fetch_add(&failures, 1);
    const char *slash = strrchr(file, '/');  // CMake передаёт полный путь, хватит имени
    if (slash) {
        file = slash + 1;
    }
    int tty = isatty(2);
    // одним dprintf, чтобы строки разных потоков не перемешивались
    dprintf(2, "%scarwash-mon: soft assert failed: %s (%s), %s:%d in %s()%s\n",
            tty ? "\033[1;33m" : "", cond, msg, file, line, func, tty ? "\033[0m" : "");
    return 0;
}

long SoftAssertFailures(void) {
    return atomic_load(&failures);
}
