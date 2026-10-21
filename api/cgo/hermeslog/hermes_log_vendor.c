//go:build cgo && !radiod

/* Outside radiod the package carries its own hermes_log: the vendored copy,
 * byte-identical to mercury's (radiod's check-symbols compares them). */
#include "../../../vendor/hermes_log/hermes_log.c"
