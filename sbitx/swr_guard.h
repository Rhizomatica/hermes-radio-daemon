/* The SWR protection's decision, apart from the hardware so it can be
 * tested: fed one bridge reading at a time, it says when to trip. */
#ifndef SWR_GUARD_H_
#define SWR_GUARD_H_

#include <stdbool.h>
#include <stdint.h>

/* Bridge readings (raw ADC counts) below this are noise: about 0.05 W.
 * Voice and RADE have gaps in their envelope where both directions read
 * next to nothing; such a reading says nothing about the antenna. */
#define SWR_GUARD_FLOOR 40

/* SWR * 10 reported when the reflected reading is at or over the forward. */
#define SWR_GUARD_MAX_X10 100

/* SWR * 10 from the bridge's forward and reflected readings. */
static inline uint32_t swr_guard_swr_x10(uint32_t vfwd, uint32_t vref)
{
    if (vref >= vfwd)
        return vfwd == vref && vfwd == 0 ? 10 : SWR_GUARD_MAX_X10;
    uint32_t swr = (10 * (vfwd + vref)) / (vfwd - vref);
    return swr > SWR_GUARD_MAX_X10 ? SWR_GUARD_MAX_X10 : swr;
}

/* One reading. A reading over the limit adds to *count, one under it
 * takes one away, and a reading in the noise leaves it alone: a signal
 * with gaps (voice, RADE) into a bad antenna must still trip, which a
 * count that started over at every quiet reading never did. True when
 * *count passes trip_after; the caller then unkeys. */
static inline bool swr_guard_step(uint16_t *count, uint32_t vfwd, uint32_t vref,
                                  uint32_t limit_x10, uint16_t trip_after)
{
    if (vfwd < SWR_GUARD_FLOOR && vref < SWR_GUARD_FLOOR)
        return false;
    if (swr_guard_swr_x10(vfwd, vref) > limit_x10)
        (*count)++;
    else if (*count > 0)
        (*count)--;
    return *count > trip_after;
}

#endif /* SWR_GUARD_H_ */
