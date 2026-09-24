/* Hand-written config.h for the vendored Opus DNN subset (see README).
 * Mirrors the float, no-RTCD parts of what Opus's ./configure produces;
 * SIMD comes from compile-time flags (SSE2 on x86-64, NEON on aarch64). */
#ifndef OPUS_DNN_CONFIG_H
#define OPUS_DNN_CONFIG_H

#define OPUS_BUILD
#define VAR_ARRAYS 1
#define FLOAT_APPROX 1
#define HAVE_LRINT 1
#define HAVE_LRINTF 1
#define ENABLE_HARDENING 1
#define ENABLE_DEEP_PLC 1
#define DISABLE_DEBUG_FLOAT 1
#define restrict __restrict

#endif
