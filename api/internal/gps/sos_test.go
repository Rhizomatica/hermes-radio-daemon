package gps_test

import (
	"context"
	"os"
	"path/filepath"
	"slices"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/gps"
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
	// The handler gets the time the SOS was set; each station is then
	// called by name, over its own transport only.
	want := []string{
		"nncp-exec -quiet gw dec_beacon <sent>",
		"uux -r -j - B0AB!dec_beacon <sent>",
		"start hermes-nncp-call -autotoss gw",
		"start uucico -m -S B0AB",
	}
	if !slices.Equal(sentArgs(t, calls), want) {
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
	if len(calls) != 8 || sentArgs(t, calls)[4] != want[0] || in[4][1] != 31 || in[5][1] != 31 {
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
	if !slices.Equal(sentArgs(t, calls), []string{"uux -r -j - B0AB!dec_beacon <sent>", "start uucico -m -S B0AB"}) || in[0][1] != 1 {
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

// sentArgs is calls with a handler's send-time argument (unix ms, within
// the last minute) written <sent>.
func sentArgs(t *testing.T, calls []string) []string {
	t.Helper()
	out := slices.Clone(calls)
	for i, c := range out {
		f := strings.Fields(c)
		if len(f) == 0 || !strings.Contains(c, "dec_beacon") {
			continue
		}
		ms, err := strconv.ParseInt(f[len(f)-1], 10, 64)
		if err != nil || time.Since(time.UnixMilli(ms)).Abs() > time.Minute {
			t.Errorf("%q: no send time as the handler's argument", c)
			continue
		}
		out[i] = strings.Join(append(f[:len(f)-1], "<sent>"), " ")
	}
	return out
}

// An SOS set while a call is in progress waits for it, then calls; the
// SOS's own calls go one at a time.
func TestSosWaitsForTheCallInProgress(t *testing.T) {
	e, q := sosEnv(t)
	poll := gps.SosCallPoll
	gps.SosCallPoll = 10 * time.Millisecond
	t.Cleanup(func() { gps.SosCallPoll = poll })
	busy := filepath.Join(transport.ProcDir, "4242")
	if err := os.MkdirAll(busy, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(busy, "comm"), []byte("uucico\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	e.Do("PATCH", "/gps/beacon", `{"sos_recipients":["B0AB"]}`)
	e.Do("POST", "/gps/sos", "")
	if calls, _ := q.waitFor(2, 100*time.Millisecond); len(calls) != 1 {
		t.Fatalf("while a call runs: ran %q, want the job only", calls)
	}
	if err := os.RemoveAll(busy); err != nil {
		t.Fatal(err)
	}
	if calls, _ := q.wait(2); len(calls) != 2 || calls[1] != "start uucico -m -S B0AB" {
		t.Errorf("after the call ended: ran %q", calls)
	}
}

// Two clears at the same moment send one "emergency over".
func TestSosClearedTwiceAtOnce(t *testing.T) {
	e, q := sosEnv(t)
	e.Do("PATCH", "/gps/beacon", `{"sos_recipients":["B0AB"]}`)
	e.Do("POST", "/gps/sos", "")
	q.wait(2)
	var wg sync.WaitGroup
	for range 2 {
		wg.Add(1)
		go func() { defer wg.Done(); e.Do("DELETE", "/gps/sos", "") }()
	}
	wg.Wait()
	if calls, _ := q.waitFor(5, sosQuiet); len(calls) != 4 {
		t.Errorf("two clears ran %q, want one more job and its call", calls[2:])
	}
}

// Without the broadcast plane an SOS still goes to its recipients; with
// neither, it is refused (503), as before.
func TestSosWithoutThePlane(t *testing.T) {
	e, q := sosEnv(t)
	e.Radio.SetBroadcast(rad.Broadcast{}, nil) // the plane stopped
	if r := e.Do("POST", "/gps/sos", ""); r.Status != 503 {
		t.Fatalf("no plane, no recipients: POST /gps/sos = %d %s", r.Status, r.Body)
	}
	e.Do("PATCH", "/gps/beacon", `{"sos_recipients":["B0AB"]}`)
	r := e.Do("POST", "/gps/sos", "")
	if j := r.JSON(t); r.Status != 202 || j["emergency"] != 1.0 || j["running"] != false {
		t.Fatalf("no plane, a recipient: POST /gps/sos = %d %s", r.Status, r.Body)
	}
	if calls, _ := q.wait(2); len(calls) != 2 {
		t.Errorf("ran %q", calls)
	}
}
