/**
 * @file memory.h
 * @brief realloc, после которого не надо проверять NULL.
 */
#ifndef MEMORY_H
#define MEMORY_H

#include <stdlib.h>
#include <unistd.h>

/// Как realloc, но без памяти сразу выходит с кодом 5: продолжать всё равно нечем.
static inline void *CheckedRealloc(void *ptr, size_t size) {
    void *p = realloc(ptr, size);
    if (p == NULL) {
        static const char msg[] = "carwash-mt: out of memory\n";
        if (write(2, msg, sizeof(msg) - 1) < 0) {
            // печатать уже некуда
        }
        _exit(5);
    }
    return p;
}

#endif
