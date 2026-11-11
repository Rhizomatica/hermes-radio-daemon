package gps_test

import (
	"context"
	"os"
	"path/filepath"
	"slices"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/transport"
)

// queued records the transports' programs and what each was given.
type queued struct {
	mu    sync.Mutex
	calls []string
	stdin [][]byte
}

func (q *queued) Run(ctx context.Context, stdin []byte, name string, args ...string) ([]byte, error) {
	q.mu.Lock()
	defer q.mu.Unlock()
	q.calls = append(q.calls, strings.Join(append([]string{name}, args...), " "))
	q.stdin = append(q.stdin, stdin)
	return nil, nil
}

func (q *queued) Start(name string, args ...string) error {
	q.mu.Lock()
	defer q.mu.Unlock()
	q.calls = append(q.calls, strings.Join(append([]string{"start", name}, args...), " "))
	q.stdin = append(q.stdin, nil)
	return nil
}

// sosWait is how long a test waits for the SOS, queued in the background;
// sosQuiet, how long it waits to see that nothing more runs.
const (
	sosWait  = 2 * time.Second
	sosQuiet = 200 * time.Millisecond
)

// wait returns the programs run once want of them have run, or what ran by
// sosWait.
func (q *queued) wait(want int) ([]string, [][]byte) { return q.waitFor(want, sosWait) }

func (q *queued) waitFor(want int, d time.Duration) ([]string, [][]byte) {
	for end := time.Now().Add(d); ; time.Sleep(time.Millisecond) {
		q.mu.Lock()
		if len(q.calls) >= want || time.Now().After(end) {
			defer q.mu.Unlock()
			return slices.Clone(q.calls), slices.Clone(q.stdin)
		}
		q.mu.Unlock()
	}
}

// sosEnv is a station with a fixed position and the plane running, whose
// UUCP reaches B0AA (alias gw) and B0AB, and whose NNCP reaches gw: B0AA
// goes over NNCP (preferred), B0AB over UUCP.
func sosEnv(t *testing.T) (*testapi.Env, *queued) {
	t.Helper()
	dir := t.TempDir()
	transport.UUCPSys = filepath.Join(dir, "sys")
	transport.UUCPConfig = filepath.Join(dir, "config")
	transport.NNCPNeigh = filepath.Join(dir, "neigh")
	transport.NNCPCallsign = filepath.Join(dir, "nncp-callsign")
	transport.ProcDir = filepath.Join(dir, "proc")
	for _, d := range []string{transport.ProcDir, filepath.Join(transport.NNCPNeigh, "gw")} {
		if err := os.MkdirAll(d, 0o755); err != nil {
			t.Fatal(err)
		}
	}
	if err := os.WriteFile(transport.UUCPSys, []byte("system B0AA\nalias gw\nsystem B0AB\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	q := &queued{}
	transport.NewRunner = func() transport.Runner { return q }
	e := testapi.New(t)
	running(e)
	if r := e.Do("PATCH", "/gps/beacon", `{"callsign":"B0AC","station_latitude":4.5,"station_longitude":-75.7}`); r.Status != 200 {
		t.Fatalf("PATCH = %d %s", r.Status, r.Body)
	}
	return e, q
}

func TestSosRecipientsSetting(t *testing.T) {
	e, _ := sosEnv(t)
	r := e.Do("PATCH", "/gps/beacon", `{"sos_recipients":["gw","B0AB"]}`)
	if r.Status != 200 {
		t.Fatalf("PATCH = %d %s", r.Status, r.Body)
	}
	got := e.Do("GET", "/gps/beacon", "").JSON(t)["sos_recipients"].([]any)
	if len(got) != 2 || got[0] != "gw" || got[1] != "B0AB" {
		t.Errorf("sos_recipients = %v", got)
	}
	for _, bad := range []string{
		`{"sos_recipients":["B0ZZ"]}`,      // no transport reaches it
		`{"sos_recipients":["B0AA","gw"]}`, // the same station twice
		`{"sos_recipients":["B0 AA"]}`,     // not a name
		`{"sos_recipients":"B0AA"}`,        // not a list
		`{"sos_recipients":["a","b","c","d","e","f","g","h","i"]}`,
	} {
		if r := e.Do("PATCH", "/gps/beacon", bad); r.Status != 422 {
			t.Errorf("PATCH %s = %d, want 422", bad, r.Status)
		}
	}
	if r := e.Do("PATCH", "/gps/beacon", `{"sos_recipients":[]}`); r.Status != 200 ||
		len(r.JSON(t)["sos_recipients"].([]any)) != 0 {
		t.Errorf("clearing the list = %d %s", r.Status, r.Body)
	}
}

func TestSosGoesToItsRecipients(t *testing.T) {
	e, q := sosEnv(t)
	e.Do("PATCH", "/gps/beacon", `{"sos_recipients":["gw","B0AB"]}`)
	if r := e.Do("POST", "/gps/sos", `{"code":2}`); r.Status != 202 {
		t.Fatalf("POST /gps/sos = %d %s", r.Status, r.Body)
	}
	calls, in := q.wait(4)
	want := []string{
		"nncp-exec -quiet gw dec_beacon",
		"uux -r -j - B0AB!dec_beacon",
		"start uucico -m -r1",
		"start hermes-nncp-call --all -autotoss",
	}
	if !slices.Equal(calls, want) {
		t.Fatalf("ran %q, want %q", calls, want)
	}
	for i := range 2 {
		if len(in[i]) != 14 || in[i][1] != 2 {
			t.Errorf("%s got % x, want the compact beacon with code 2", calls[i], in[i])
		}
	}

	// Clearing it sends "emergency over" the same way.
	if r := e.Do("DELETE", "/gps/sos", ""); r.Status != 200 {
		t.Fatalf("DELETE /gps/sos = %d %s", r.Status, r.Body)
	}
	calls, in = q.wait(8)
	if len(calls) != 8 || calls[4] != want[0] || in[4][1] != 31 || in[5][1] != 31 {
		t.Errorf("after clearing: ran %q", calls)
	}
	// Clearing again, with no emergency set, sends nothing.
	e.Do("DELETE", "/gps/sos", "")
	if calls, _ := q.waitFor(9, sosQuiet); len(calls) != 8 {
		t.Errorf("clearing twice ran %q", calls[8:])
	}
}

func TestSosOneRecipientIsCalledByName(t *testing.T) {
	e, q := sosEnv(t)
	e.Do("PATCH", "/gps/beacon", `{"sos_recipients":["B0AB"]}`)
	e.Do("POST", "/gps/sos", "")
	calls, in := q.wait(2)
	if !slices.Equal(calls, []string{"uux -r -j - B0AB!dec_beacon", "start uucico -m -S B0AB"}) || in[0][1] != 1 {
		t.Errorf("ran %q with % x", calls, in[0])
	}
}

func TestSosWithoutPositionQueuesNothing(t *testing.T) {
	e, q := sosEnv(t)
	e.Do("PATCH", "/gps/beacon", `{"sos_recipients":["B0AB"],"station_latitude":null,"station_longitude":null}`)
	if r := e.Do("POST", "/gps/sos", ""); r.Status != 202 {
		t.Fatalf("POST /gps/sos = %d", r.Status)
	}
	if calls, _ := q.waitFor(1, sosQuiet); len(calls) != 0 {
		t.Errorf("ran %q with nothing to send", calls)
	}
}
