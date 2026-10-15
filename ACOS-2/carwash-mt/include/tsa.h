/**
 * @file tsa.h
 * @brief Аннотации Clang Thread Safety Analysis (-Wthread-safety).
 *
 * Под clang макросы раскрываются в атрибуты, и компилятор статически проверяет,
 * что поля с GUARDED_BY читаются и пишутся только под захваченным мьютексом.
 * Под другими компиляторами макросы пустые.
 */
#ifndef TSA_H
#define TSA_H

#ifdef __clang__
#define TSA_ATTR(x) __attribute__((x))
#else
#define TSA_ATTR(x)
#endif

/// Помечает тип как «мьютекс» для анализатора.
#define CAPABILITY(x) TSA_ATTR(capability(x))
/// Поле можно трогать только при захваченном мьютексе @p x.
#define GUARDED_BY(x) TSA_ATTR(guarded_by(x))
/// То же для данных, на которые указывает поле-указатель.
#define PT_GUARDED_BY(x) TSA_ATTR(pt_guarded_by(x))
/// Функцию можно звать только при захваченных мьютексах.
#define REQUIRES(...) TSA_ATTR(requires_capability(__VA_ARGS__))
/// Функция захватывает мьютексы и не отпускает их.
#define ACQUIRE(...) TSA_ATTR(acquire_capability(__VA_ARGS__))
/// Функция отпускает захваченные мьютексы.
#define RELEASE(...) TSA_ATTR(release_capability(__VA_ARGS__))
/// Функцию нельзя звать, держа эти мьютексы.
#define EXCLUDES(...) TSA_ATTR(locks_excluded(__VA_ARGS__))
/// Отключает анализ внутри функции (обёртки над pthread, где захват не виден анализатору).
#define NO_TSA TSA_ATTR(no_thread_safety_analysis)

#endif
