/* celt_fatal() for the vendored subset. Opus defines it in celt/celt.c,
 * which is the whole CELT codec; the definition itself lives in
 * celt/arch.h behind CELT_C, so take it from there. */
#define CELT_C
#include "config.h"
#include "arch.h"
