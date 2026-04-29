/* hermes-radio-daemon - configuration utilities
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

#ifndef CFG_UTILS_H_
#define CFG_UTILS_H_

#include <stdbool.h>
#include <pthread.h>
#include <stddef.h>

#include "radio.h"
#include <iniparser.h>

/* Initialize config subsystem and start the writer thread */
bool cfg_init(radio *radio_h, const char *cfg_radio, const char *cfg_user,
              pthread_t *config_tid);

/* Read just the backend selection early, before daemon threads start */
bool cfg_detect_backend(const char *cfg_radio, radio_backend_kind *backend_kind,
                        char *controller_path, size_t controller_path_len);
radio_backend_kind cfg_backend_kind_from_string(const char *backend_name);

/* Signal the writer thread to exit and wait */
bool cfg_shutdown(radio *radio_h, pthread_t *config_tid);

/* Thread-safe wrapper around iniparser_set */
int cfg_set(radio *radio_h, dictionary *ini, const char *entry, const char *val);

/* Load hardware/radio config from INI file */
bool init_config_radio(radio *radio_h, const char *ini_name);

/* Load user/profile config from INI file */
bool init_config_user(radio *radio_h, const char *ini_name);

/* Write hardware/radio config back to disk */
bool write_config_radio(radio *radio_h, const char *ini_name);

/* Write user/profile config back to disk */
bool write_config_user(radio *radio_h, const char *ini_name);

/* Free config dictionaries */
bool close_config_radio(radio *radio_h);
bool close_config_user(radio *radio_h);

/* Background thread that flushes dirty configs every ~2 s */
void *config_thread(void *radio_h_v);

#endif /* CFG_UTILS_H_ */
