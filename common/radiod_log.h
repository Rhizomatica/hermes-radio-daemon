/* radiod_log - radiod's logging: mercury's hermes_log (vendor/hermes_log,
 * or the copy linked in with mercury), set up from core.ini.
 *
 * Log with hermes_log.h's HLOGD/HLOGT/HLOGI/HLOGW/HLOGE(component, ...), one
 * component per module ("radiod", "hamlib", "sbitx", "ws", ...). Lines go
 * to stderr (journald under systemd) and, with main:log_file, to a file.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RADIOD_LOG_H_
#define RADIOD_LOG_H_

#include <stdbool.h>

#include "hermes_log.h"
#include "radio.h"

/* Start the logger, at INFO. First thing in main: hermes_log drops
 * whatever is logged before it starts. */
void radiod_log_start(void);

/* Apply main:log_level, main:log_component_levels, main:log_file and
 * main:log_jsonl. Bad values are reported and left at their defaults. */
void radiod_log_configure(const radio *radio_h);

/* Write out what is queued and stop the logger. Last thing in main. */
void radiod_log_stop(void);

/* "trace", "debug", "timing", "info", "warn" or "error" (any case); false
 * otherwise. */
bool radiod_log_level(const char *name, hermes_log_level_t *out);

/* Parse "component=level, component=level" and call set() for each pair.
 * Returns the number of pairs set, or -1 at the first bad pair (those
 * before it are set). */
int radiod_log_components(const char *spec,
                          void (*set)(const char *component, hermes_log_level_t level, void *user),
                          void *user);

#endif /* RADIOD_LOG_H_ */
