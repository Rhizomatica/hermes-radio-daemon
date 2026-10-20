//go:build cgo && !radiod

package hermeslog_test

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
	"time"

	"github.com/Rhizomatica/hermes-radio-daemon/api/cgo/hermeslog"
)

func run(t *testing.T, jsonl bool, min hermeslog.Level, log func()) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "hermes.log")
	if err := hermeslog.Init(0); err != nil {
		t.Fatal(err)
	}
	hermeslog.SetLevel(hermeslog.Error) // keep the test's stderr quiet
	if err := hermeslog.SetFile(path, min, jsonl); err != nil {
		t.Fatal(err)
	}
	if s := hermeslog.FileStatus(); !strings.Contains(s, path) {
		t.Errorf("FileStatus = %q", s)
	}
	log()
	hermeslog.Shutdown()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return string(b)
}

// The lines are hermes_log's own: the clock, the uptime, the level, the
// component, the message.
func TestTextLines(t *testing.T) {
	got := run(t, false, hermeslog.Debug, func() {
		hermeslog.Log(hermeslog.Info, "test", "hello")
		hermeslog.Logf(hermeslog.Warn, "test", "n=%d %s", 3, "x")
		hermeslog.Log(hermeslog.Debug, "test.sub", "detail")
		hermeslog.Log(hermeslog.Timing, "test", "tick")
		hermeslog.Log(hermeslog.Error, "test", "bad")
	})
	want := []string{"[INF] [test] hello", "[WRN] [test] n=3 x", "[DBG] [test.sub] detail",
		"[TMG] [test] tick", "[ERR] [test] bad"}
	line := regexp.MustCompile(`^\d\d:\d\d:\d\d\.\d{3} \[\+\d+\.\d{3}s\] (\[[A-Z]{3}\] \[[a-z.]+\] .*)$`)
	var msgs []string
	for _, l := range strings.Split(strings.TrimSpace(got), "\n") {
		m := line.FindStringSubmatch(l)
		if m == nil {
			t.Errorf("not a hermes_log line: %q", l)
			continue
		}
		if strings.Contains(m[1], "[log]") { // hermes_log's own "writing log to"
			continue
		}
		msgs = append(msgs, m[1])
	}
	if strings.Join(msgs, "\n") != strings.Join(want, "\n") {
		t.Errorf("lines:\n%s\nwant:\n%s", strings.Join(msgs, "\n"), strings.Join(want, "\n"))
	}
}

func TestJSONL(t *testing.T) {
	got := run(t, true, hermeslog.Info, func() {
		hermeslog.Log(hermeslog.Info, "api", `a "quoted" word`)
	})
	if !strings.Contains(got, `"lv":"INF","c":"api","m":"a \"quoted\" word"}`) {
		t.Errorf("JSONL = %s", got)
	}
}

// The file's minimum level holds back the lines below it.
func TestFileMinimum(t *testing.T) {
	got := run(t, false, hermeslog.Warn, func() {
		hermeslog.Log(hermeslog.Info, "test", "not this")
		hermeslog.Log(hermeslog.Warn, "test", "this")
	})
	if strings.Contains(got, "not this") || !strings.Contains(got, "[WRN] [test] this") {
		t.Errorf("file = %s", got)
	}
}

func TestSetFileError(t *testing.T) {
	if err := hermeslog.Init(0); err != nil {
		t.Fatal(err)
	}
	defer hermeslog.Shutdown()
	hermeslog.SetLevel(hermeslog.Error)
	err := hermeslog.SetFile(filepath.Join(t.TempDir(), "no", "such", "dir", "x.log"), hermeslog.Info, false)
	if err == nil || !strings.Contains(err.Error(), "cannot open log file") {
		t.Errorf("SetFile into a missing directory: %v", err)
	}
}

func TestLevels(t *testing.T) {
	for i, s := range []string{"debug", "timing", "info", "warn", "error"} {
		l, err := hermeslog.ParseLevel(s)
		if err != nil || int(l) != i || l.String() != s {
			t.Errorf("ParseLevel(%q) = %v, %v", s, l, err)
		}
	}
	if _, err := hermeslog.ParseLevel("loud"); err == nil {
		t.Error("ParseLevel accepted loud")
	}
}

func TestUptime(t *testing.T) {
	if err := hermeslog.Init(0); err != nil {
		t.Fatal(err)
	}
	defer hermeslog.Shutdown()
	time.Sleep(20 * time.Millisecond)
	if u := hermeslog.Uptime(); u < 20*time.Millisecond || u > 5*time.Second {
		t.Errorf("Uptime = %v", u)
	}
	if d := hermeslog.Dropped(); d != 0 {
		t.Errorf("Dropped = %d", d)
	}
}
