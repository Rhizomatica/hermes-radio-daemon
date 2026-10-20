// Package logging is the API layer's logging: log/slog records go to
// radiod's logger (mercury's hermes_log) through the radiod C API, so the Go
// layer's lines have radiod's format, file sink and levels:
//
//	19:32:12.668 [+0.306s] [INF] [api.http] PATCH /api/v2/radio/profiles/0 200 1.2ms request_id=4f2a...
//
// The record's message comes first, then its attributes as key=value. Levels
// map onto hermes_log's: slog's Debug, Info, Warn and Error, and LevelTiming
// for hermes_log's TIMING. Which lines are written is radiod's configuration
// (main:log_level, main:log_component_levels), not the API layer's.
package logging

import (
	"context"
	"fmt"
	"log/slog"
	"strconv"
	"strings"
	"unicode"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
)

// LevelTiming is hermes_log's TIMING level, between Debug and Info.
const LevelTiming = slog.Level(-2)

// Sink takes finished lines: the radio (radiod's logger), or a test's stub.
type Sink interface {
	Log(level rad.LogLevel, component, message string)
}

// New returns a logger whose lines carry the component tag, e.g. "api.http".
func New(sink Sink, component string) *slog.Logger {
	return slog.New(&handler{sink: sink, component: component})
}

type handler struct {
	sink      Sink
	component string
	attrs     string // preformatted " key=value" pairs from With
	group     string // prefix from WithGroup, "a.b."
}

// Enabled always says yes: radiod's logger filters by level and component.
func (h *handler) Enabled(context.Context, slog.Level) bool { return true }

func (h *handler) Handle(_ context.Context, r slog.Record) error {
	var b strings.Builder
	b.WriteString(r.Message)
	b.WriteString(h.attrs)
	r.Attrs(func(a slog.Attr) bool {
		appendAttr(&b, h.group, a)
		return true
	})
	h.sink.Log(level(r.Level), h.component, b.String())
	return nil
}

func (h *handler) WithAttrs(attrs []slog.Attr) slog.Handler {
	var b strings.Builder
	b.WriteString(h.attrs)
	for _, a := range attrs {
		appendAttr(&b, h.group, a)
	}
	c := *h
	c.attrs = b.String()
	return &c
}

func (h *handler) WithGroup(name string) slog.Handler {
	if name == "" {
		return h
	}
	c := *h
	c.group = h.group + name + "."
	return &c
}

func level(l slog.Level) rad.LogLevel {
	switch {
	case l < LevelTiming:
		return rad.LogDebug
	case l < slog.LevelInfo:
		return rad.LogTiming
	case l < slog.LevelWarn:
		return rad.LogInfo
	case l < slog.LevelError:
		return rad.LogWarn
	default:
		return rad.LogError
	}
}

func appendAttr(b *strings.Builder, group string, a slog.Attr) {
	a.Value = a.Value.Resolve()
	if a.Equal(slog.Attr{}) {
		return
	}
	if a.Value.Kind() == slog.KindGroup {
		prefix := group
		if a.Key != "" {
			prefix += a.Key + "."
		}
		for _, ga := range a.Value.Group() {
			appendAttr(b, prefix, ga)
		}
		return
	}
	b.WriteByte(' ')
	b.WriteString(group)
	b.WriteString(a.Key)
	b.WriteByte('=')
	b.WriteString(value(a.Value))
}

// value quotes a value that would not read back as one token.
func value(v slog.Value) string {
	s := fmt.Sprint(v.Any())
	if v.Kind() == slog.KindString {
		s = v.String()
	}
	if s == "" || strings.IndexFunc(s, func(r rune) bool { return unicode.IsSpace(r) || r == '"' || r == '=' }) >= 0 {
		return strconv.Quote(s)
	}
	return s
}
