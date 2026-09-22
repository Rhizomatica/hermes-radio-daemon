/* Minimal Linux os_interop shim for the vendored mercury ring buffer.
 * Provides only the mutex/cond macros + headers ring_buffer_posix.c uses.
 * (Mercury's full os_interop.h is a Windows/Linux shim we don't need here.) */
#pragma once
#include <pthread.h>
#include <sys/shm.h>     /* SHMLBA */
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#define MUTEX_LOCK(x)            pthread_mutex_lock(x)
#define MUTEX_UNLOCK(x)          pthread_mutex_unlock(x)
#define COND_WAIT(x, y)          pthread_cond_wait(x, y)
#define COND_TIMED_WAIT(x, y, z) pthread_cond_timedwait(x, y, z)
#define COND_SIGNAL(x)           pthread_cond_signal(x)
