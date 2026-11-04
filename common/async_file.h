/* async_file - file I/O for the real-time threads, done by a thread of its own
 *
 * The codec and DSP threads must never wait on a file: a stat, an open or a
 * read, even on tmpfs, can take milliseconds, and a missed period is silence
 * on the air. These objects do the file side in a thread of their own and
 * give the real-time side only a lock-free ring (one producer, one
 * consumer): copying in or out of it is all it does.
 *
 * async_writer: what the real-time thread writes goes to a file. It is
 * written only while the writer is on; what does not fit in the ring is
 * dropped and counted, never waited for. A writer with a trigger path turns
 * itself on while that file exists (the test hooks' convention).
 *
 * async_loop: a file read in a loop, as a test signal. The thread keeps the
 * ring full, following the path: deleting the file ends the loop, replacing
 * it changes it.
 *
 * Copyright (C) 2026 Rhizomatica
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef ASYNC_FILE_H_
#define ASYNC_FILE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct async_writer async_writer;
typedef struct async_loop async_loop;

/* A writer named name (for its log lines), with a ring of ring_bytes. With
 * trigger and path, it writes to path while trigger exists (checked every
 * 250 ms); without, async_writer_on and _off turn it on and off. NULL if it
 * cannot be made. Not from a real-time thread. */
async_writer *async_writer_new(const char *name, size_t ring_bytes,
                               const char *trigger, const char *path);
/* Open path (truncated) and write from now on; off writes what is queued
 * and closes it. Not from a real-time thread. */
void async_writer_on(async_writer *w, const char *path);
void async_writer_off(async_writer *w);
/* From the real-time thread: n bytes queued while on (dropped otherwise),
 * or as many as fit. Never blocks. */
void async_writer_write(async_writer *w, const void *data, size_t n);
/* Bytes dropped for want of room in the ring, since the writer was made. */
uint64_t async_writer_dropped(async_writer *w);
/* Close and free (what is queued is written first). */
void async_writer_free(async_writer *w);

/* A loop over path, with a ring of ring_bytes. NULL if it cannot be made.
 * Not from a real-time thread. */
async_loop *async_loop_new(const char *path, size_t ring_bytes);
/* From the real-time thread: n bytes of the loop into out, or false (and
 * nothing) when the file is not there or the ring is short of n. Never
 * blocks. */
bool async_loop_read(async_loop *l, void *out, size_t n);
/* From the real-time thread, in the periods it does not read: lets a file
 * removed or replaced meanwhile be taken up before the next read. Never
 * blocks. */
void async_loop_idle(async_loop *l);
void async_loop_free(async_loop *l);

#endif /* ASYNC_FILE_H_ */
