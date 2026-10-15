/**
 * @file unit.h
 * @brief Мини-харнесс модульных тестов: CHECK, RUN, UnitSummary.
 */
#ifndef UNIT_H
#define UNIT_H

#include <stdio.h>

static int unitChecks = 0;
static int unitFailed = 0;

/// Проверяет условие; при провале печатает файл:строку и считает ошибку.
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++unitChecks;                                                                              \
        if (!(cond)) {                                                                             \
            ++unitFailed;                                                                          \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                      \
        }                                                                                          \
    } while (0)

/// Запускает тест-функцию и печатает её имя.
#define RUN(fn)                                                                                    \
    do {                                                                                           \
        int before_ = unitFailed;                                                                  \
        fn();                                                                                      \
        printf("%s %s\n", unitFailed == before_ ? "PASS" : "FAIL", #fn);                           \
    } while (0)

/// Итог прогона. @return 0, если провалов нет, иначе 1 (код выхода программы)
static inline int UnitSummary(void) {
    printf("%d checks, %d failed\n", unitChecks, unitFailed);
    return unitFailed ? 1 : 0;
}

#endif
