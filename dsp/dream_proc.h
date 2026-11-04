/* dream_proc - a Dream (DRM) process behind two pipes
 *
 * Dream runs as a child in a directory of its own, where its Dream.ini is
 * written first (Dream reads ./Dream.ini and takes no path), with its stdin
 * and stdout as raw s16le streams. Two threads move the samples, so the
 * DSP thread never waits on a pipe: it pushes into one ring and pops from
 * the other. What does not fit is dropped, and counted.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DREAM_PROC_H_
#define DREAM_PROC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct dream_proc dream_proc;

/* Start dream_path with args (NULL-terminated, the program name first),
 * in dir (made if missing) with ini as its Dream.ini, or ini NULL to leave
 * what is there. ring is each ring's size in samples. NULL on failure. */
dream_proc *dream_proc_start(const char *dream_path, const char *dir, const char *ini,
                             const char *const args[], size_t ring);

/* Queue n samples for Dream's stdin; returns how many fit. */
size_t dream_proc_write(dream_proc *p, const int16_t *s, size_t n);

/* Take up to n samples Dream wrote; returns how many there were. */
size_t dream_proc_read(dream_proc *p, int16_t *s, size_t n);

/* Samples waiting to be read. */
size_t dream_proc_readable(dream_proc *p);

/* End Dream's input once what is queued has gone: the transmitter sends
 * out its last frames and exits. */
void dream_proc_finish(dream_proc *p);

/* Whether the process is still there. */
bool dream_proc_running(dream_proc *p);

/* Samples dropped because a ring was full: into Dream, and out of it. */
void dream_proc_dropped(dream_proc *p, uint64_t *in, uint64_t *out);

/* Stop it (SIGTERM, then SIGKILL after 2 s) and free everything. */
void dream_proc_stop(dream_proc *p);

/* Whether dream_path's --help lists option, for an option an older Dream
 * would refuse to start with. Waits up to 3 s: not for the DSP thread. */
bool dream_proc_has_option(const char *dream_path, const char *option);

#endif /* DREAM_PROC_H_ */
