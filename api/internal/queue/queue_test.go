package queue_test

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/queue"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/transport"
)

// fake answers uustat and nncp-stat and records the rest.
type fake struct {
	mu    sync.Mutex
	calls []string
}

const (
	uustatOut = "B0AA.C00A6 B0AA root 11-06 15:23 Executing rmail a@b (sending 1234 bytes)\n" +
		"B0AB.C00A9 B0AB root 11-06 16:00 Executing dec_message (sending 99 bytes)\n"
	nncpStatOut = "gw\n\ttx ABC 1 KiB (nice: B)\n"
)

func (f *fake) Run(ctx context.Context, stdin []byte, name string, args ...string) ([]byte, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls = append(f.calls, strings.Join(append([]string{name}, args...), " "))
	switch name {
	case "uustat":
		if len(args) > 0 && args[0] == "-a" {
			return []byte(uustatOut), nil
		}
	case "nncp-stat":
		return []byte(nncpStatOut), nil
	}
	return nil, nil
}

func (f *fake) Start(name string, args ...string) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls = append(f.calls, strings.Join(append([]string{"start", name}, args...), " "))
	return nil
}

func (f *fake) last() string {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.calls[len(f.calls)-1]
}

// env is a station with UUCP (B0AA alias gw, B0AB) and NNCP (gw).
func env(t *testing.T) (*testapi.Env, *fake) {
	t.Helper()
	dir := t.TempDir()
	transport.UUCPSys = filepath.Join(dir, "sys")
	transport.UUCPConfig = filepath.Join(dir, "config")
	transport.NNCPNeigh = filepath.Join(dir, "neigh")
	transport.NNCPCallsign = filepath.Join(dir, "nncp-callsign")
	transport.ProcDir = filepath.Join(dir, "proc")
	for _, d := range []string{transport.ProcDir, filepath.Join(transport.NNCPNeigh, "gw"), filepath.Join(transport.NNCPNeigh, "self")} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(transport.UUCPSys, []byte("system B0AA\nalias gw\nsystem B0AB\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	f := &fake{}
	queue.NewRunner = func() transport.Runner { return f }
	return testapi.New(t), f
}

func TestListQueue(t *testing.T) {
	e, _ := env(t)
	r := e.Do("GET", "/queue", "")
	if r.Status != 200 {
		t.Fatalf("GET /queue = %d %s", r.Status, r.Body)
	}
	data := r.JSON(t)["data"].([]any)
	if len(data) != 3 {
		t.Fatalf("jobs = %v", data)
	}
	first := data[0].(map[string]any)
	if first["job_id"] != "uucp.B0AA.C00A6" || first["transport"] != "uucp" || first["kind"] != "mail" ||
		first["size_bytes"].(float64) != 1234 || first["queued_at"] == nil || first["mail_to"].([]any)[0] != "a@b" {
		t.Errorf("first = %v", first)
	}
	last := data[2].(map[string]any)
	if last["job_id"] != "nncp.gw.ABC" || last["station"] != "B0AA" || last["queued_at"] != nil {
		t.Errorf("NNCP job = %v", last)
	}

	r = e.Do("GET", "/queue?station=B0AB", "")
	if d := r.JSON(t)["data"].([]any); len(d) != 1 || d[0].(map[string]any)["station"] != "B0AB" {
		t.Errorf("filtered = %s", r.Body)
	}
	if r := e.Do("GET", "/queue?station=a%20b", ""); r.Status != 422 {
		t.Errorf("bad station = %d", r.Status)
	}
}

func TestCancelJob(t *testing.T) {
	e, f := env(t)
	if r := e.Do("DELETE", "/queue/uucp.B0AA.C00A6", ""); r.Status != 204 || f.last() != "uustat -k B0AA.C00A6" {
		t.Errorf("DELETE UUCP job = %d, ran %q", r.Status, f.last())
	}
	if r := e.Do("DELETE", "/queue/nncp.gw.ABC", ""); r.Status != 204 || f.last() != "nncp-rm -node gw -pkt" {
		t.Errorf("DELETE NNCP job = %d, ran %q", r.Status, f.last())
	}
	if r := e.Do("DELETE", "/queue/nncp.gw.NOPE", ""); r.Status != 404 {
		t.Errorf("DELETE an unknown job = %d", r.Status)
	}
}

func TestCallNowAndStop(t *testing.T) {
	e, f := env(t)
	if r := e.Do("POST", "/queue/call", `{"station":"gw"}`); r.Status != 202 || f.last() != "start hermes-nncp-call -autotoss gw" {
		t.Errorf("call gw = %d, ran %q", r.Status, f.last())
	}
	if r := e.Do("POST", "/queue/call", `{"station":"gw","transport":"uucp"}`); r.Status != 202 || f.last() != "start uucico -m -S B0AA" {
		t.Errorf("call gw over UUCP = %d, ran %q", r.Status, f.last())
	}
	if r := e.Do("POST", "/queue/call", `{"station":"B0AB","transport":"nncp"}`); r.Status != 422 {
		t.Errorf("NNCP to a UUCP-only station = %d", r.Status)
	}
	if r := e.Do("POST", "/queue/call", `{"station":"B0ZZ"}`); r.Status != 404 {
		t.Errorf("an unknown station = %d", r.Status)
	}
	if r := e.Do("POST", "/queue/call", `{"transport":"reticulum"}`); r.Status != 422 || r.JSON(t)["code"] != "not_supported" {
		t.Errorf("reticulum = %d %s", r.Status, r.Body)
	}
	if r := e.Do("POST", "/queue/call", ""); r.Status != 202 {
		t.Errorf("call everyone = %d", r.Status)
	}

	// A call in progress: 409, and stopping is fine either way.
	pid := filepath.Join(transport.ProcDir, "999999")
	if err := os.MkdirAll(pid, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(pid, "comm"), []byte("uucico\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	if r := e.Do("POST", "/queue/call", `{"station":"B0AB"}`); r.Status != 409 || r.JSON(t)["code"] != "busy" {
		t.Errorf("call while one runs = %d %s", r.Status, r.Body)
	}
	if r := e.Do("POST", "/queue/stop", ""); r.Status != 204 {
		t.Errorf("stop = %d", r.Status)
	}
}
