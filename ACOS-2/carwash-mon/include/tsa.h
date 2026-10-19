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
/// GUARDED_BY для поля структуры, которое охраняет соседнее поле-мьютекс. Apple clang 21 в C не
/// видит в атрибуте поля имена соседних полей: `int x GUARDED_BY(m)` он связывает не с s->m, а с
/// неким отдельным «m» и ругается на каждый доступ даже под захваченным s->m. Поэтому под Apple
/// clang пометка пустая (проверка таких полей пропадает, REQUIRES(st->m) у функций остаётся),
/// а clang из Homebrew (LLVM 23) и clang-tidy проверяют поля по-настоящему.
#ifdef __apple_build_version__
#define MEMBER_GUARDED_BY(x)
#else
#define MEMBER_GUARDED_BY(x) GUARDED_BY(x)
#endif
/// Функцию можно звать только при захваченных мьютексах.
#define REQUIRES(...) TSA_ATTR(requires_capability(__VA_ARGS__))
/// Функция захватывает мьютексы и не отпускает их.
#define ACQUIRE(...) TSA_ATTR(acquire_capability(__VA_ARGS__))
/// Функция отпускает захваченные мьютексы.
#define RELEASE(...) TSA_ATTR(release_capability(__VA_ARGS__))
/// Функция захватывает rwlock для чтения (общий доступ).
#define ACQUIRE_SHARED(...) TSA_ATTR(acquire_shared_capability(__VA_ARGS__))
/// Функция отпускает rwlock, захваченный для чтения.
#define RELEASE_SHARED(...) TSA_ATTR(release_shared_capability(__VA_ARGS__))
/// Функцию можно звать, держа мьютексы хотя бы для чтения.
#define REQUIRES_SHARED(...) TSA_ATTR(requires_shared_capability(__VA_ARGS__))
/// Функция отпускает rwlock независимо от режима захвата (чтение или запись).
#define RELEASE_GENERIC(...) TSA_ATTR(release_generic_capability(__VA_ARGS__))
/// Функцию нельзя звать, держа эти мьютексы.
#define EXCLUDES(...) TSA_ATTR(locks_excluded(__VA_ARGS__))
/// Отключает анализ внутри функции (обёртки над pthread, где захват не виден анализатору).
#define NO_TSA TSA_ATTR(no_thread_safety_analysis)

#endif
