package broadcasts_test

import (
	"bytes"
	"fmt"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing"
	"time"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/broadcasts"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
)

type env struct {
	*testapi.Env
	tx, rx string
}

func newEnv(t *testing.T) env {
	e := testapi.New(t)
	tx, rx := filepath.Join(t.TempDir(), "tx"), filepath.Join(t.TempDir(), "rx")
	os.MkdirAll(tx, 0o755)
	os.MkdirAll(rx, 0o755)
	e.Radio.SetBroadcast(rad.Broadcast{Running: true, Connected: true, ModeName: "DATAC3", TxDir: tx, RxDir: rx}, nil)
	return env{e, tx, rx}
}

// upload POSTs a file the way a client does: the body is the file.
func (e env) upload(query string, body []byte) testapi.Response {
	req := httptest.NewRequest("POST", "/api/v2/broadcasts?"+query, bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/octet-stream")
	w := httptest.NewRecorder()
	e.R.ServeHTTP(w, req)
	return testapi.Response{Status: w.Code, Header: w.Header(), Body: w.Body.Bytes()}
}

func TestNotRunning(t *testing.T) {
	e := testapi.New(t)
	for _, p := range []string{"/broadcasts", "/broadcasts/received", "/broadcasts/x.bin"} {
		if r := e.Do("GET", p, ""); r.Status != 503 {
			t.Errorf("GET %s with the plane off = %d, want 503", p, r.Status)
		}
	}
}

func TestQueue(t *testing.T) {
	e := newEnv(t)
	data := bytes.Repeat([]byte("hermes "), 300) // 2100 bytes
	r := e.upload("name=notice.txt&passes=3", data)
	if r.Status != 201 {
		t.Fatalf("POST = %d %s", r.Status, r.Body)
	}
	f := r.JSON(t)
	perPass := e.Radio.BroadcastFramesPerPass("notice.txt", uint64(len(data)))
	wantID := "notice-" + strconv.FormatInt(3*perPass, 10) + "_frames.txt"
	if f["id"] != wantID || f["name"] != "notice.txt" || f["state"] != "waiting" ||
		f["passes"].(float64) != 3 || f["frames_limit"].(float64) != float64(3*perPass) {
		t.Fatalf("queued = %v, want id %s", f, wantID)
	}
	if got, err := os.ReadFile(filepath.Join(e.tx, wantID)); err != nil || !bytes.Equal(got, data) {
		t.Fatalf("the spool file: %v", err)
	}
	if ents, _ := os.ReadDir(e.tx); len(ents) != 1 {
		t.Errorf("tx holds %d entries, want just the file (no upload left behind)", len(ents))
	}

	// Another one, and the first goes on the air: it is listed first.
	time.Sleep(10 * time.Millisecond)
	if r := e.upload("name=second.bin", []byte("x")); r.Status != 201 {
		t.Fatalf("second POST = %d %s", r.Status, r.Body)
	}
	b, _ := e.Radio.Broadcast()
	b.Path, b.State, b.FramesSent, b.Pass = filepath.Join(e.tx, wantID), rad.BroadcastSending, 5, 1
	e.Radio.SetBroadcast(b, nil)
	j := e.Do("GET", "/broadcasts", "").JSON(t)
	q := j["data"].([]any)
	if j["running"] != true || j["mode"] != "DATAC3" || len(q) != 2 {
		t.Fatalf("GET /broadcasts = %v", j)
	}
	first := q[0].(map[string]any)
	if first["id"] != wantID || first["state"] != "sending" || first["frames_sent"].(float64) != 5 || first["pass"].(float64) != 1 {
		t.Errorf("the one on the air = %v", first)
	}
	if g := e.Do("GET", "/broadcasts/"+wantID, ""); g.Status != 200 || g.JSON(t)["state"] != "sending" {
		t.Errorf("GET one = %d %s", g.Status, g.Body)
	}

	if r := e.upload("name=notice.txt", data); r.Status != 409 {
		t.Errorf("the same name again = %d, want 409", r.Status)
	}
	if r := e.Do("DELETE", "/broadcasts/"+wantID, ""); r.Status != 204 {
		t.Fatalf("DELETE = %d %s", r.Status, r.Body)
	}
	if _, err := os.Stat(filepath.Join(e.tx, wantID)); !os.IsNotExist(err) {
		t.Error("the file is still in the spool")
	}
	if r := e.Do("DELETE", "/broadcasts/"+wantID, ""); r.Status != 404 {
		t.Errorf("DELETE again = %d, want 404", r.Status)
	}
	if r := e.Do("GET", "/broadcasts/nothing.bin", ""); r.Status != 404 {
		t.Errorf("GET a file not queued = %d, want 404", r.Status)
	}
}

func TestCreateChecks(t *testing.T) {
	e := newEnv(t)
	for q, want := range map[string]int{
		"name=":                  422,
		"name=.hidden":           422,
		"name=a/b":               422,
		"name=x-500_frames.bin":  422, // a budget is passes' job
		"name=ok.bin&passes=0":   422,
		"name=ok.bin&passes=17":  422,
		"name=ok.bin&passes=two": 422,
	} {
		if r := e.upload(q, []byte("data")); r.Status != want {
			t.Errorf("POST ?%s = %d, want %d", q, r.Status, want)
		}
	}
	if r := e.upload("name=empty.bin", nil); r.Status != 422 {
		t.Errorf("an empty file = %d, want 422", r.Status)
	}
	if r := e.upload("name=big.bin", make([]byte, broadcasts.MaxBytes+1)); r.Status != 413 {
		t.Errorf("too large = %d, want 413", r.Status)
	}
}

func TestRateLimit(t *testing.T) {
	e := newEnv(t)
	for i := 0; i < broadcasts.PerHour; i++ {
		if r := e.upload("name=f"+strconv.Itoa(i)+".bin", []byte("x")); r.Status != 201 {
			t.Fatalf("upload %d = %d %s", i, r.Status, r.Body)
		}
	}
	r := e.upload("name=one-too-many.bin", []byte("x"))
	if r.Status != 429 || r.Header.Get("Retry-After") == "" {
		t.Errorf("upload %d = %d, want 429 with Retry-After", broadcasts.PerHour+1, r.Status)
	}
}

func TestReceived(t *testing.T) {
	e := newEnv(t)
	old, newer := filepath.Join(e.rx, "old.txt"), filepath.Join(e.rx, "bulletin.pdf")
	os.WriteFile(old, []byte("old"), 0o644)
	os.WriteFile(newer, []byte("%PDF bulletin"), 0o644)
	os.WriteFile(filepath.Join(e.rx, ".incoming"), []byte("partial"), 0o644)
	past := time.Now().Add(-time.Hour)
	os.Chtimes(old, past, past)

	j := e.Do("GET", "/broadcasts/received", "").JSON(t)
	d := j["data"].([]any)
	if len(d) != 2 || d[0].(map[string]any)["name"] != "bulletin.pdf" || d[1].(map[string]any)["name"] != "old.txt" {
		t.Fatalf("received = %v (hidden files left out, newest first)", j)
	}

	r := e.Do("GET", "/broadcasts/received/bulletin.pdf", "")
	if r.Status != 200 || string(r.Body) != "%PDF bulletin" || r.Header.Get("Content-Type") != "application/octet-stream" ||
		!strings.Contains(r.Header.Get("Content-Disposition"), "bulletin.pdf") {
		t.Fatalf("download = %d %q %v", r.Status, r.Body, r.Header)
	}
	for _, p := range []string{"/broadcasts/received/missing.bin", "/broadcasts/received/.incoming"} {
		if r := e.Do("GET", p, ""); r.Status != 404 {
			t.Errorf("GET %s = %d, want 404", p, r.Status)
		}
	}
	if r := e.Do("DELETE", "/broadcasts/received/old.txt", ""); r.Status != 204 {
		t.Fatalf("DELETE = %d %s", r.Status, r.Body)
	}
	if _, err := os.Stat(old); !os.IsNotExist(err) {
		t.Error("old.txt is still there")
	}
}

// The broadcasts topic: off while the plane is; its snapshot is the queue,
// and a file queued arrives as an event.
func TestBroadcastsTopic(t *testing.T) {
	off := testapi.New(t)
	w := off.Open()
	if h := w.Next(testapi.Type("hello"), time.Second); strings.Contains(fmt.Sprint(h["topics"]), "broadcasts") {
		t.Errorf("hello offers broadcasts with the plane off: %v", h["topics"])
	}

	e := newEnv(t)
	w = e.Open()
	if h := w.Next(testapi.Type("hello"), time.Second); !strings.Contains(fmt.Sprint(h["topics"]), "broadcasts") {
		t.Fatalf("hello does not offer broadcasts: %v", h["topics"])
	}
	w.Send(`{"type":"subscribe","id":"s","topics":["broadcasts"]}`)
	isTopic := func(m map[string]any) bool { return m["type"] == "event" && m["topic"] == "broadcasts" }
	snap := w.Next(isTopic, 2*time.Second)
	d := snap["data"].(map[string]any)
	if d["running"] != true || d["mode"] != "DATAC3" || len(d["queue"].([]any)) != 0 {
		t.Fatalf("snapshot = %v", d)
	}
	if r := e.upload("name=notice.txt", []byte("for the whole area")); r.Status != 201 {
		t.Fatalf("POST = %d %s", r.Status, r.Body)
	}
	ev := w.Next(isTopic, 3*time.Second)
	q, _ := ev["data"].(map[string]any)["queue"].([]any)
	if len(q) != 1 || q[0].(map[string]any)["name"] != "notice.txt" {
		t.Errorf("event = %v, want the queued file", ev)
	}
}
