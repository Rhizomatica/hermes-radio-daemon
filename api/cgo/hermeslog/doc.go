// Package hermeslog is the Go binding of hermes_log, mercury's logger, which
// radiod logs through too: the same line format, levels per component, and
// file sink (text or JSONL) as the C code's HLOGD/HLOGT/HLOGI/HLOGW/HLOGE.
//
//	19:32:12.668 [+0.306s] [INF] [api.http] PATCH /api/v2/radio/profiles/0 200
//
// Inside radiod (build tag radiod) it calls radiod's own logger, which
// radiod starts and configures from core.ini: Go code only logs. In any
// other Go program it compiles the vendored hermes_log
// (vendor/hermes_log/hermes_log.c) and the program starts it with Init and
// stops it with Shutdown; nothing is logged before Init.
//
// The package needs cgo (a C compiler).
package hermeslog
