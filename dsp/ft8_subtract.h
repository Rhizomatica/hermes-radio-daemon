/* ft8_subtract - take a decoded FT8 signal out of the audio
 *
 * A strong signal hides the weaker ones under and beside it. Once it is
 * decoded its waveform is known: this rebuilds it (GFSK, BT 2, as WSJT-X
 * sends it), finds where it sits to a sample and a fraction of a hertz,
 * follows its amplitude and phase as the channel fades them, and subtracts
 * it, as WSJT-X's subtractft8 does. The decoder then runs again on what is
 * left.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef FT8_SUBTRACT_H
#define FT8_SUBTRACT_H

#include <stdbool.h>
#include <stdint.h>

#define FT8_WAVEFORM_SAMPLES (79 * 1920)

/* The signal of these 79 tones at 12 kHz, tone 0 at freq_hz, unit
 * amplitude: FT8_WAVEFORM_SAMPLES samples into out. */
void ft8_waveform(const uint8_t tones[79], float freq_hz, float *out);

/* Subtract the signal of these 79 tones from 12 kHz audio. freq_hz (tone
 * 0) and time_s (its start, in seconds from audio[0]) are where the decoder
 * found it, to within a few hertz and 0.2 s. False when it is not there. */
bool ft8_subtract(float *audio, int n, const uint8_t tones[79], float freq_hz, float time_s);

#endif /* FT8_SUBTRACT_H */
