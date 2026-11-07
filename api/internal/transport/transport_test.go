package transport

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"sync"
	"testing"
	"time"
)

// fakeRunner records each program run and answers from out.
type fakeRunner struct {
	mu    sync.Mutex
	calls []string // "name arg arg" (Start: "start name arg")
	stdin map[string][]byte
	out   map[string]string // by program name
	err   map[string]error
}

func newFake() *fakeRunner {
	return &fakeRunner{stdin: map[string][]byte{}, out: map[string]string{}, err: map[string]error{}}
}

func (f *fakeRunner) Run(ctx context.Context, stdin []byte, name string, args ...string) ([]byte, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	line := strings.Join(append([]string{name}, args...), " ")
	f.calls = append(f.calls, line)
	f.stdin[name] = stdin
	return []byte(f.out[name]), f.err[name]
}

func (f *fakeRunner) Start(name string, args ...string) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls = append(f.calls, strings.Join(append([]string{"start", name}, args...), " "))
	return f.err[name]
}

func (f *fakeRunner) last() string {
	f.mu.Lock()
	defer f.mu.Unlock()
	if len(f.calls) == 0 {
		return ""
	}
	return f.calls[len(f.calls)-1]
}

// station sets up a station's configuration in a temporary directory: the
// UUCP sys file (nil: no UUCP) and NNCP's neighbours (nil: no NNCP).
func station(t *testing.T, sys *string, neigh []string) {
	t.Helper()
	dir := t.TempDir()
	UUCPSys = filepath.Join(dir, "sys")
	UUCPConfig = filepath.Join(dir, "config")
	NNCPNeigh = filepath.Join(dir, "neigh")
	NNCPCallsign = filepath.Join(dir, "nncp-callsign")
	OutDir = filepath.Join(dir, "out")
	ProcDir = filepath.Join(dir, "proc")
	if err := os.MkdirAll(ProcDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if sys != nil {
		write(t, UUCPSys, *sys)
		write(t, UUCPConfig, "nodename B0AH\n")
	}
	if neigh != nil {
		for _, n := range append(neigh, nncpSelf) {
			if err := os.MkdirAll(filepath.Join(NNCPNeigh, n), 0o755); err != nil {
				t.Fatal(err)
			}
		}
		write(t, NNCPCallsign, "B0AH\n")
	}
}

func write(t *testing.T, path, s string) {
	t.Helper()
	if err := os.WriteFile(path, []byte(s), 0o644); err != nil {
		t.Fatal(err)
	}
}

const sysFile = `# the stations
system B0AA
alias gw
port HF
system B0AB
alias b0ab-radio
`

func ptr(s string) *string { return &s }

func TestStationsMergeAcrossTransports(t *testing.T) {
	// NNCP names B0AA by its UUCP alias, "gw": one station, reached both ways.
	station(t, ptr(sysFile), []string{"gw", "B0AC"})
	l := New(Settings{}, newFake())
	got := l.Stations()
	if len(got) != 3 {
		t.Fatalf("stations = %+v", got)
	}
	gw := got[0]
	if gw.Name != "B0AA" || !reflect.DeepEqual(gw.Via, []Kind{UUCP, NNCP}) ||
		gw.Addr[UUCP] != "B0AA" || gw.Addr[NNCP] != "gw" || !reflect.DeepEqual(gw.Aliases, []string{"gw"}) {
		t.Errorf("B0AA = %+v", gw)
	}
	if got[1].Name != "B0AB" || !reflect.DeepEqual(got[1].Via, []Kind{UUCP}) {
		t.Errorf("B0AB = %+v", got[1])
	}
	if got[2].Name != "B0AC" || !reflect.DeepEqual(got[2].Via, []Kind{NNCP}) {
		t.Errorf("B0AC = %+v", got[2])
	}
	if l.Self() != "B0AH" {
		t.Errorf("Self = %q", l.Self())
	}
	if !reflect.DeepEqual(l.Kinds(), []Kind{UUCP, NNCP}) {
		t.Errorf("Kinds = %v", l.Kinds())
	}
}

// Per station reached: the one transport that reaches it, or the preferred
// one where both do.
func TestRoute(t *testing.T) {
	station(t, ptr(sysFile), []string{"gw", "B0AC"})
	for _, tc := range []struct {
		prefer     Kind
		dest       string
		want, addr string
	}{
		{"", "gw", "nncp", "gw"}, // NNCP by default
		{"", "B0AA", "nncp", "gw"},
		{UUCP, "gw", "uucp", "B0AA"},
		{NNCP, "B0AB", "uucp", "B0AB"}, // only UUCP reaches it
		{UUCP, "B0AC", "nncp", "B0AC"}, // only NNCP reaches it
		{"", "b0ab-radio", "uucp", "B0AB"},
	} {
		k, _, addr, err := New(Settings{Prefer: tc.prefer}, newFake()).Route(tc.dest)
		if err != nil || string(k) != tc.want || addr != tc.addr {
			t.Errorf("prefer %q: Route(%q) = %q %q %v, want %q %q", tc.prefer, tc.dest, k, addr, err, tc.want, tc.addr)
		}
	}
	l := New(Settings{}, newFake())
	if _, _, _, err := l.Route("B0ZZ"); !errors.Is(err, ErrUnknownStation) {
		t.Errorf("unknown station: %v", err)
	}
	if _, _, _, err := l.Route("gw;rm"); !errors.Is(err, ErrBadName) {
		t.Errorf("bad name: %v", err)
	}
}

func TestExecOverEachTransport(t *testing.T) {
	station(t, ptr(sysFile), []string{"gw"})
	f := newFake()
	f.out["uux"] = "B0AB.C00A6\n"
	l := New(Settings{}, f)
	frame := []byte{0xE0, 1, 2, 3}

	id, err := l.Exec(context.Background(), "B0AB", "dec_beacon", nil, frame)
	if err != nil || id != "uucp.B0AB.C00A6" {
		t.Fatalf("Exec over UUCP = %q %v", id, err)
	}
	if f.last() != "uux -r -j - B0AB!dec_beacon" || !reflect.DeepEqual(f.stdin["uux"], frame) {
		t.Errorf("uux run as %q with %v", f.last(), f.stdin["uux"])
	}

	if _, err := l.Exec(context.Background(), "B0AA", "dec_beacon", []string{"-s"}, frame); err != nil {
		t.Fatal(err)
	}
	if f.last() != "nncp-exec -quiet gw dec_beacon -s" || !reflect.DeepEqual(f.stdin["nncp-exec"], frame) {
		t.Errorf("nncp-exec run as %q", f.last())
	}

	for _, bad := range [][]string{{"a b"}, {"$(x)"}, {"-"}, {"a;b"}} {
		if _, err := l.Exec(context.Background(), "B0AB", "dec_beacon", bad, nil); !errors.Is(err, ErrBadName) {
			t.Errorf("args %q: %v", bad, err)
		}
	}
	if _, err := l.Exec(context.Background(), "B0AB", "../bin/sh", nil, nil); !errors.Is(err, ErrBadName) {
		t.Errorf("handler with a path: %v", err)
	}
}

func TestSendFileOverEachTransport(t *testing.T) {
	station(t, ptr(sysFile), []string{"gw"})
	f := newFake()
	f.out["uucp"] = "B0AB.C00A7\n"
	l := New(Settings{}, f)
	data := []byte("a message")

	id, err := l.SendFile(context.Background(), "B0AB", "m1.hmp", data)
	if err != nil || id != "uucp.B0AB.C00A7" {
		t.Fatalf("SendFile over UUCP = %q %v", id, err)
	}
	if !strings.HasPrefix(f.last(), "uucp -r -j -C "+OutDir+"/.out-") || !strings.HasSuffix(f.last(), " B0AB!~/m1.hmp") {
		t.Errorf("uucp run as %q", f.last())
	}
	if ents, _ := os.ReadDir(OutDir); len(ents) != 0 {
		t.Errorf("the copy for uucp was left behind: %v", ents)
	}

	if _, err := l.SendFile(context.Background(), "gw", "m1.hmp", data); err != nil {
		t.Fatal(err)
	}
	if f.last() != "nncp-file -quiet - gw:m1.hmp" || string(f.stdin["nncp-file"]) != "a message" {
		t.Errorf("nncp-file run as %q", f.last())
	}
	for _, bad := range []string{"../x", ".hidden", "a/b", "-x", ""} {
		if _, err := l.SendFile(context.Background(), "B0AB", bad, data); !errors.Is(err, ErrBadName) {
			t.Errorf("name %q: %v", bad, err)
		}
	}
}

func TestParseUustat(t *testing.T) {
	now := time.Date(2026, 11, 6, 20, 0, 0, 0, time.Local)
	jobs := parseUustat(`B0AA.C00A6 B0AA root 11-06 15:23 Executing rmail a@b c@d (sending 1234 bytes)
B0AA.C00A7 B0AA root 11-06 15:24 Executing dec_beacon (sending 14 bytes)
B0AB.C00A8 B0AB www-data 12-31 23:59 Sending /var/www/photo.jpg (5678 bytes) to ~/photo.jpg
B0AB.C00A9 B0AB root 11-06 16:00 Executing dec_message (sending 99 bytes)
junk
`, now)
	if len(jobs) != 4 {
		t.Fatalf("jobs = %+v", jobs)
	}
	j := jobs[0]
	if j.ID != "uucp.B0AA.C00A6" || j.Station != "B0AA" || j.Kind != "mail" || j.Bytes != 1234 ||
		!reflect.DeepEqual(j.MailTo, []string{"a@b", "c@d"}) || j.QueuedAt.Format("2006-01-02 15:04") != "2026-11-06 15:23" {
		t.Errorf("mail job = %+v", j)
	}
	if jobs[1].Kind != "gps" || jobs[1].Bytes != 14 {
		t.Errorf("beacon job = %+v", jobs[1])
	}
	if jobs[2].Kind != "file" || jobs[2].QueuedAt.Year() != 2025 { // December is last year's
		t.Errorf("file job = %+v", jobs[2])
	}
	if jobs[3].Kind != "message" {
		t.Errorf("message job = %+v", jobs[3])
	}
}

func TestParseNncpStat(t *testing.T) {
	jobs := parseNncpStat(`gw
	tx ABCDEF234567 2.4 MiB (nice: B)
	rx ZZZZ 1 KiB (nice: B)
B0AC
	tx QWERTY 512 B (nice: B)
`)
	if len(jobs) != 2 {
		t.Fatalf("jobs = %+v", jobs)
	}
	if jobs[0].ID != "nncp.gw.ABCDEF234567" || jobs[0].Bytes != 2516582 || jobs[0].Station != "gw" {
		t.Errorf("first = %+v", jobs[0])
	}
	if jobs[1].ID != "nncp.B0AC.QWERTY" || jobs[1].Bytes != 512 {
		t.Errorf("second = %+v", jobs[1])
	}
}

// The queue merges both, oldest first, under the stations' names.
func TestQueueAndCancel(t *testing.T) {
	station(t, ptr(sysFile), []string{"gw"})
	f := newFake()
	f.out["uustat"] = "B0AB.C00A9 B0AB root 11-06 16:00 Executing dec_message (sending 99 bytes)\n" +
		"B0AA.C00A6 B0AA root 11-06 15:23 Executing rmail a@b (sending 1234 bytes)\n"
	f.out["nncp-stat"] = "gw\n\ttx ABC 1 KiB (nice: B)\n"
	l := New(Settings{}, f)
	jobs, err := l.Queue(context.Background())
	if err != nil || len(jobs) != 3 {
		t.Fatalf("Queue = %+v %v", jobs, err)
	}
	if jobs[0].ID != "uucp.B0AA.C00A6" || jobs[1].ID != "uucp.B0AB.C00A9" || jobs[2].ID != "nncp.gw.ABC" {
		t.Errorf("order = %s %s %s", jobs[0].ID, jobs[1].ID, jobs[2].ID)
	}
	if jobs[2].Station != "B0AA" { // NNCP's gw is the station B0AA
		t.Errorf("NNCP job's station = %q", jobs[2].Station)
	}

	if err := l.Cancel(context.Background(), "uucp.B0AA.C00A6"); err != nil || f.last() != "uustat -k B0AA.C00A6" {
		t.Errorf("Cancel UUCP: %v, ran %q", err, f.last())
	}
	if err := l.Cancel(context.Background(), "nncp.gw.ABC"); err != nil || f.last() != "nncp-rm -node gw -pkt" ||
		string(f.stdin["nncp-rm"]) != "ABC\n" {
		t.Errorf("Cancel NNCP: %v, ran %q", err, f.last())
	}
	for _, id := range []string{"nncp.gw.NOPE", "smtp.gw.X", "uucp", "uucp.a b.c"} {
		if err := l.Cancel(context.Background(), id); !errors.Is(err, ErrNotFound) {
			t.Errorf("Cancel(%q) = %v", id, err)
		}
	}
}

func TestCall(t *testing.T) {
	station(t, ptr(sysFile), []string{"gw"})
	f := newFake()
	l := New(Settings{Prefer: UUCP, UucicoAirOptions: []string{"-Y"}, UucicoTCPSystems: []string{"B0AA"}}, f)

	if _, err := l.Call("B0AB", ""); err != nil || f.last() != "start uucico -m -Y -S B0AB" {
		t.Errorf("call over the air: %v, ran %q", err, f.last())
	}
	if _, err := l.Call("gw", ""); err != nil || f.last() != "start uucico -m -S B0AA" { // over TCP: no -Y
		t.Errorf("call over TCP: %v, ran %q", err, f.last())
	}
	if _, err := l.Call("gw", NNCP); err != nil || f.last() != "start hermes-nncp-call -autotoss gw" {
		t.Errorf("call over NNCP: %v, ran %q", err, f.last())
	}
	if _, err := l.Call("B0AB", NNCP); !errors.Is(err, ErrNotSupported) {
		t.Errorf("NNCP to a UUCP-only station: %v", err)
	}
	f.calls = nil
	if _, err := l.Call("", ""); err != nil ||
		!reflect.DeepEqual(f.calls, []string{"start uucico -m -r1", "start hermes-nncp-call --all -autotoss"}) {
		t.Errorf("call everyone: %v, ran %q", err, f.calls)
	}

	// A call in progress: busy, and Stop finds it.
	pidDir := filepath.Join(ProcDir, "999999")
	if err := os.MkdirAll(pidDir, 0o755); err != nil {
		t.Fatal(err)
	}
	write(t, filepath.Join(pidDir, "comm"), "uucico\n")
	if _, err := l.Call("B0AB", ""); !errors.Is(err, ErrBusy) {
		t.Errorf("call while one runs: %v", err)
	}
	write(t, filepath.Join(pidDir, "comm"), "hermes-nncp-cal\n")
	if !l.Calling() {
		t.Error("an NNCP call is not seen")
	}
}

func TestNoTransport(t *testing.T) {
	station(t, nil, nil)
	l := New(Settings{}, newFake())
	if len(l.Kinds()) != 0 || len(l.Stations()) != 0 || l.Self() != "" {
		t.Errorf("a station without UUCP or NNCP: %v %v %q", l.Kinds(), l.Stations(), l.Self())
	}
	if jobs, err := l.Queue(context.Background()); err != nil || len(jobs) != 0 {
		t.Errorf("Queue = %v %v", jobs, err)
	}
}
