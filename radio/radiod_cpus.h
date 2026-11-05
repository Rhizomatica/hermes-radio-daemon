/* radiod_cpus - the CPUs radiod may run on, from before -c pins it to one
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef RADIOD_CPUS_H_
#define RADIOD_CPUS_H_

#include <sched.h>      /* cpu_set_t: define _GNU_SOURCE before any include */
#include <stdbool.h>

/* The process's CPU set before the -c pin, as the threads that must stay
 * off the DSP core (mercury, the FT8 decoder) use it; false before radiod
 * has read it. */
bool radiod_unpinned_cpus(cpu_set_t *out);

#endif /* RADIOD_CPUS_H_ */
