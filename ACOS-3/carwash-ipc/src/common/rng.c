#include "rng.h"

/// Перемешивание splitmix32: близкие входы дают далёкие состояния.
static uint32_t SplitMix(uint32_t x) {
    uint32_t z = x + 0x9E3779B9U;
    z = (z ^ (z >> 16U)) * 0x85EBCA6BU;
    z = (z ^ (z >> 13U)) * 0xC2B2AE35U;
    return z ^ (z >> 15U);
}

void RngInit(Rng *r, uint32_t seed, uint32_t stream) {
    uint32_t s = SplitMix(seed ^ (stream * 0x9E3779B9U));
    r->s = s ? s : 1;  // xorshift не выходит из нуля
}

/// Marsaglia, "Xorshift RNGs", 2003
static uint32_t Next(Rng *r) {
    r->s ^= r->s << 13U;
    r->s ^= r->s >> 17U;
    r->s ^= r->s << 5U;
    return r->s;
}

int RngRange(Rng *r, int lo, int hi) {
    if (hi <= lo) {
        return lo;
    }
    return lo + (int)(Next(r) % (uint32_t)(hi - lo + 1));
}

int RngChance(Rng *r, double p) {
    return (Next(r) / 4294967296.0) < p;
}
