//go:build cgo && radiod

package hermeslog

// Inside radiod, hermes_log's symbols are radiod's, resolved when
// radio_daemon is linked; cgo's own check link must not insist on them.

// #cgo LDFLAGS: -Wl,--unresolved-symbols=ignore-all
import "C"
