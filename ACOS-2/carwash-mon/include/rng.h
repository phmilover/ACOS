/**
 * @file rng.h
 * @brief Генератор xorshift32 на сущность: у каждого потока свой поток чисел.
 *
 * Состояние выводится из seed и номера потока (splitmix32), поэтому результат
 * не зависит от порядка выполнения потоков. Потоки: координатор stream=1,
 * пост stream=100+s*MAX_POSTS+p, машина stream=1000+id.
 */
#ifndef RNG_H
#define RNG_H

#include <stdint.h>

/// Состояние одного генератора; принадлежит одному потоку, блокировок не нужно.
typedef struct {
    uint32_t s;
} Rng;

/**
 * @brief Готовит генератор.
 * @param r      генератор
 * @param seed   общий seed прогона
 * @param stream номер потока чисел; разные номера дают независимые последовательности
 */
void RngInit(Rng *r, uint32_t seed, uint32_t stream);
/// Случайное число из [lo, hi]; при hi <= lo возвращает lo.
int RngRange(Rng *r, int lo, int hi);
/// 1 с вероятностью @p p, иначе 0.
int RngChance(Rng *r, double p);

#endif
