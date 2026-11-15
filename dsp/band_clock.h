/* band_clock - the station clock's error, as the FT8 and JS8 stations it
 * hears show it.
 *
 * Every FT8 or JS8 decode carries where the transmission started against
 * where this station's clock says its slot started. One station's timing
 * scatters, but stations whose own clocks are right scatter around this
 * station's error, so the median of a group of them reads that error off
 * the band. It costs nothing: the decoders have measured every number.
 * A wrong clock is the commonest reason FT8 and JS8 calls go unanswered,
 * and HERMES stations often run without the internet to set it.
 *
 * Only stations on the UTC slots count. HERMES stations send FT8 when a
 * message is queued, off the slots, so their offsets scatter over the
 * whole period: the estimate is the median of the largest group of
 * offsets that agree within BAND_CLOCK_CLUSTER_S, and only when that
 * group is big enough to stand out from such traffic.
 *
 * The idea is sdroxide's (crates/sdroxide-digi/src/clock.rs).
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef BAND_CLOCK_H_
#define BAND_CLOCK_H_

#include <stdbool.h>

/* Decodes remembered (FT8 at its busiest: about 40 a slot, 8 slots). */
#define BAND_CLOCK_MAX_DECODES 512
/* The estimate is made from the decodes this many seconds before the
 * newest: eight FT8 slots, long enough to outvote one loud station whose
 * own clock is off, short enough to follow a clock being corrected. */
#define BAND_CLOCK_WINDOW_S    120.0
/* With no decode for this long the estimate is dropped: the band went
 * quiet or the radio left it, and the clock may have been set since. */
#define BAND_CLOCK_KEEP_S      600.0
/* Offsets that agree within this span are one group. FT8 decoders measure
 * the start to a few tens of ms and stations' own sound cards add a few
 * hundred, so stations on the slots fall within half a second. */
#define BAND_CLOCK_CLUSTER_S   0.5
/* The fewest decodes in that group for an estimate. */
#define BAND_CLOCK_MIN_DECODES 5
/* ...and the smallest share of all decodes it must hold (1 in this many):
 * offsets spread evenly over a 15 s period put a thirtieth of them in any
 * half second, so a third is never traffic off the slots. */
#define BAND_CLOCK_MIN_SHARE   3
/* Logged as a warning past this error, and again as fixed below
 * BAND_CLOCK_OK_S (apart, so an error near the line logs once). WSJT-X
 * asks for a clock within a second. */
#define BAND_CLOCK_WARN_S      1.0
#define BAND_CLOCK_OK_S        0.5

/* One decode: its start's offset from its slot, seconds (positive: the
 * transmission started later than this station's clock expected, so the
 * clock is fast), heard at at_s (Unix seconds, this station's clock). */
void band_clock_add(double offset_s, double at_s);

/* The estimate at now_s, from the decodes then remembered: true with the
 * error in seconds and the decodes it rests on; false when there is none. */
bool band_clock_estimate(double now_s, double *offset_s, int *decodes);

/* band_clock_estimate() now. */
bool band_clock_get(double *offset_s, int *decodes);

/* Forget every decode (tests). */
void band_clock_reset(void);

#endif /* BAND_CLOCK_H_ */
