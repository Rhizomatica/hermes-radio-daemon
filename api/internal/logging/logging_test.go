package logging_test

import (
	"context"
	"log/slog"
	"testing"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/logging"
)

// Lines reach radiod's logger with the component, hermes_log's level and
// the attributes after the message.
func TestLines(t *testing.T) {
	s := rad.NewStub()
	l := logging.New(s, "api.http")
	l.Debug("GET /api/v2/radio 200", "ms", 0.4)
	l.Log(context.Background(), logging.LevelTiming, "tick")
	l.Info("PATCH /api/v2/radio/profiles/0 200", "request_id", "abc", "peer", "127.0.0.1")
	l.Warn("odd", "detail", "two words", "empty", "")
	l.Error("handler panicked", "panic", "index out of range")
	l.With("conn", 7).WithGroup("cmd").Info("ran", "name", "radio.set", "ok", true)

	want := []string{
		"[DBG] [api.http] GET /api/v2/radio 200 ms=0.4",
		"[TMG] [api.http] tick",
		"[INF] [api.http] PATCH /api/v2/radio/profiles/0 200 request_id=abc peer=127.0.0.1",
		`[WRN] [api.http] odd detail="two words" empty=""`,
		`[ERR] [api.http] handler panicked panic="index out of range"`,
		"[INF] [api.http] ran conn=7 cmd.name=radio.set cmd.ok=true",
	}
	got := s.Logs()
	if len(got) != len(want) {
		t.Fatalf("got %d lines, want %d: %q", len(got), len(want), got)
	}
	for i := range want {
		if got[i] != want[i] {
			t.Errorf("line %d = %q\n        want %q", i, got[i], want[i])
		}
	}
}

// slog's group attributes flatten into dotted keys, and levels between the
// named ones go to the nearest one below.
func TestGroupsAndLevels(t *testing.T) {
	s := rad.NewStub()
	l := logging.New(s, "api")
	l.Info("x", slog.Group("req", "method", "GET", slog.Group("hdr", "n", 2)))
	l.Log(context.Background(), slog.LevelInfo+2, "between info and warn")
	l.Log(context.Background(), slog.LevelDebug-2, "below debug")
	l.Log(context.Background(), logging.LevelTrace, "trace")
	want := []string{
		"[INF] [api] x req.method=GET req.hdr.n=2",
		"[INF] [api] between info and warn",
		"[DBG] [api] below debug",
		"[TRC] [api] trace",
	}
	got := s.Logs()
	for i := range want {
		if i >= len(got) || got[i] != want[i] {
			t.Errorf("line %d = %q, want %q", i, got, want[i])
		}
	}
}
