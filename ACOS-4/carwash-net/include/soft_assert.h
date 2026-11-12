/**
 * @file soft_assert.h
 * @brief Мягкая проверка: при ошибке пишет в stderr и продолжает работу.
 *
 * Обычный assert() роняет программу, и тогда пропадают журнал и статистика.
 * SOFT_ASSERT только сообщает, где нарушено условие, и возвращает его значение,
 * поэтому рядом можно обойти плохой случай:
 *
 * @code
 * if (!SOFT_ASSERT(count < MAX_QUEUE, "queue array is full")) {
 *     return;
 * }
 * @endcode
 *
 * Аудитор смотрит на линию снаружи, по событиям, а SOFT_ASSERT проверяет
 * предусловия функций изнутри. С -DNDEBUG сообщения отключаются, но условие
 * по-прежнему вычисляется, так что обход плохого случая продолжает работать.
 */
#ifndef SOFT_ASSERT_H
#define SOFT_ASSERT_H

/**
 * @brief Печатает сообщение о нарушенном условии. Напрямую не вызывается, только через SOFT_ASSERT.
 * @return всегда 0, чтобы макрос вернул "ложь"
 */
int SoftAssertFail(const char *cond, const char *msg, const char *file, int line, const char *func);

/// Сколько раз сработал SOFT_ASSERT за прогон.
long SoftAssertFailures(void);

#ifdef NDEBUG
#define SOFT_ASSERT(cond, msg) (!!(cond))
#else
/// Проверяет @p cond; если ложно, печатает @p msg с файлом и строкой. Значение - 1 или 0.
#define SOFT_ASSERT(cond, msg)                                                                     \
    ((cond) ? 1 : SoftAssertFail(#cond, (msg), __FILE__, __LINE__, __func__))
#endif

#endif
