//go:build cgo

package hermeslog

/*
#cgo CFLAGS: -I${SRCDIR}/../../../vendor/hermes_log
#include <stdlib.h>
#include "hermes_log.h"

// hermes_logf is variadic, which cgo cannot call: log a finished message.
static void hermeslog_line(int level, const char *component, const char *message)
{
	hermes_logf((hermes_log_level_t) level, component, "%s", message);
}
*/
import "C"

import (
	"errors"
	"fmt"
	"time"
	"unsafe"
)

// Level is hermes_log's level.
type Level int

const (
	Trace  Level = C.HERMES_LOG_LEVEL_TRACE  // raw decoded bytes, the most verbose internals
	Debug  Level = C.HERMES_LOG_LEVEL_DEBUG  // verbose internals
	Timing Level = C.HERMES_LOG_LEVEL_TIMING // protocol timing events
	Info   Level = C.HERMES_LOG_LEVEL_INFO   // general status
	Warn   Level = C.HERMES_LOG_LEVEL_WARN
	Error  Level = C.HERMES_LOG_LEVEL_ERROR
)

// names, from Trace (-1) up.
var names = [...]string{"trace", "debug", "timing", "info", "warn", "error"}

func (l Level) String() string {
	if l >= Trace && l <= Error {
		return names[l-Trace]
	}
	return fmt.Sprintf("Level(%d)", int(l))
}

// ParseLevel reads "trace", "debug", "timing", "info", "warn" or "error", as
// radiod's main:log_level.
func ParseLevel(s string) (Level, error) {
	for i, n := range names {
		if s == n {
			return Level(i) + Trace, nil
		}
	}
	return Info, fmt.Errorf("hermeslog: %q is not trace, debug, timing, info, warn or error", s)
}

func clamp(l Level) C.int {
	if l < Trace {
		l = Trace
	}
	if l > Error {
		l = Error
	}
	return C.int(l)
}

// Init starts the logger with room for capacity queued lines (0: the
// default). It does nothing if the logger is already running, as inside
// radiod.
func Init(capacity int) error {
	if capacity < 0 {
		capacity = 0
	}
	if C.hermes_log_init(C.size_t(capacity)) != 0 {
		return errors.New("hermeslog: the logger did not start")
	}
	return nil
}

// Shutdown writes out what is queued and stops the logger.
func Shutdown() { C.hermes_log_shutdown() }

// SetLevel sets the lowest level written to stderr.
func SetLevel(l Level) { C.hermes_log_set_level(C.hermes_log_level_t(clamp(l))) }

// SetComponentLevel sets the lowest level written to stderr for one component.
func SetComponentLevel(component string, l Level) {
	cs := C.CString(component)
	defer C.free(unsafe.Pointer(cs))
	C.hermes_log_set_component_level(cs, C.hermes_log_level_t(clamp(l)))
}

// SetFile adds (or replaces) the file sink: path opened for appending, lines
// from min up, in JSONL when jsonl is set. The error says why it failed.
func SetFile(path string, min Level, jsonl bool) error {
	cs := C.CString(path)
	defer C.free(unsafe.Pointer(cs))
	if C.hermes_log_set_file(cs, C.hermes_log_level_t(clamp(min)), C.bool(jsonl)) != 0 {
		return errors.New(FileStatus())
	}
	return nil
}

// FileStatus reports the last SetFile: where the file is, or why it is not.
func FileStatus() string {
	if s := C.hermes_log_file_status(); s != nil {
		return C.GoString(s)
	}
	return ""
}

// CloseFile closes the file sink.
func CloseFile() { C.hermes_log_close_file() }

// Dropped is how many lines were dropped because the queue was full.
func Dropped() uint64 { return uint64(C.hermes_log_dropped_count()) }

// Uptime is the time since the logger started (the "[+s.ms]" of each line).
func Uptime() time.Duration { return time.Duration(C.hermes_uptime_ms()) * time.Millisecond }

// Log logs one line as is. The logger keeps up to 479 bytes of it.
func Log(l Level, component, message string) {
	cc, cm := C.CString(component), C.CString(message)
	C.hermeslog_line(clamp(l), cc, cm)
	C.free(unsafe.Pointer(cc))
	C.free(unsafe.Pointer(cm))
}

// Logf logs one line formatted with fmt.
func Logf(l Level, component, format string, args ...any) {
	Log(l, component, fmt.Sprintf(format, args...))
}
