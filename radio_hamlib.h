/* hermes-radio-daemon - Hamlib radio backend
 *
 * Copyright (C) 2024-2025 Rhizomatica
 * Author: Rafael Diniz <rafael@riseup.net>
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3, or (at your option)
 * any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this software; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 *
 */

#ifndef RADIO_HAMLIB_H_
#define RADIO_HAMLIB_H_

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include "radio.h"

/* Initialize Hamlib and open the rig; returns true on success */
bool radio_hamlib_init(radio *radio_h);

/* Close and free the Hamlib rig */
void radio_hamlib_shutdown(radio *radio_h);

/* Set frequency on the active VFO; updates profile and config */
void set_frequency(radio *radio_h, uint32_t frequency, uint32_t profile);

/* Set mode on the active VFO; updates profile and config */
void set_mode(radio *radio_h, uint16_t mode, uint32_t profile);

/* Switch transmit/receive state */
void tr_switch(radio *radio_h, bool txrx_state);

/* Set BFO (no-op for Hamlib radios; stored in config for API compat) */
void set_bfo(radio *radio_h, uint32_t frequency);

/* Set reflected power threshold */
void set_reflected_threshold(radio *radio_h, uint32_t ref_threshold);

/* Set speaker volume (stored in profile/config) */
void set_speaker_volume(radio *radio_h, uint32_t speaker_level, uint32_t profile);

/* Set serial number */
void set_serial(radio *radio_h, uint32_t serial);

/* Set profile timeout */
void set_profile_timeout(radio *radio_h, int32_t timeout);

/* Set TX power level percentage */
void set_power_knob(radio *radio_h, uint16_t power_level, uint32_t profile);

/* Set digital voice flag */
void set_digital_voice(radio *radio_h, bool digital_voice, uint32_t profile);

/* Switch to a different profile */
void set_profile(radio *radio_h, uint32_t profile);

/* Forward power reading (from Hamlib if available, else 0) */
uint32_t get_fwd_power(radio *radio_h);

/* Reflected power / SWR reading */
uint32_t get_ref_power(radio *radio_h);
uint32_t get_swr(radio *radio_h);

/* Poll power measurements and check SWR protection */
bool update_power_measurements(radio *radio_h);
void swr_protection_check(radio *radio_h);

/* Background I/O thread (periodic profile timer, optional polling) */
void *radio_io_thread(void *radio_h_v);

/* Periodic timer helpers */
void wait_next_activation(void);
int  start_periodic_timer(uint64_t offset_us);

/* Global shutdown flag (defined in radio_daemon.c) */
extern _Atomic bool shutdown_;

/* Timer reset flag */
extern _Atomic bool timer_reset;
extern _Atomic time_t timeout_counter;

#endif /* RADIO_HAMLIB_H_ */
