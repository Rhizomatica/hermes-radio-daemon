// Package testapi helps test API handlers: a router over a radio Stub, and
// requests and hermes.v1 commands against it, with no C and no radiod.
package testapi

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/station"
)

// Env is a station router over a Stub.
type Env struct {
	T     *testing.T
	Radio *rad.Stub
	R     *api.Router
}

// New returns the whole station's router over a fresh Stub.
func New(t *testing.T) *Env {
	s := rad.NewStub()
	return &Env{T: t, Radio: s, R: station.NewRouter(s)}
}

// Response is what a request got.
type Response struct {
	Status int
	Header http.Header
	Body   []byte
}

// JSON decodes the body into a map (fails the test if it is not JSON).
func (r Response) JSON(t *testing.T) map[string]any {
	t.Helper()
	var m map[string]any
	if err := json.Unmarshal(r.Body, &m); err != nil {
		t.Fatalf("body is not a JSON object: %v\n%s", err, r.Body)
	}
	return m
}

// Do sends a request (body as JSON when not empty) under /api/v2.
func (e *Env) Do(method, path, body string) Response {
	var rd io.Reader
	if body != "" {
		rd = strings.NewReader(body)
	}
	req := httptest.NewRequest(method, "/api/v2"+path, rd)
	if body != "" {
		req.Header.Set("Content-Type", "application/json")
	}
	w := httptest.NewRecorder()
	e.R.ServeHTTP(w, req)
	return Response{w.Code, w.Header(), w.Body.Bytes()}
}

// WS is one hermes.v1 connection to the router.
type WS struct {
	e    *Env
	ID   int64
	read int // frames already returned by Next
}

var lastConn atomic.Int64

// Open opens a hermes.v1 connection; it is closed when the test ends.
func (e *Env) Open() *WS {
	e.T.Helper()
	id := lastConn.Add(1)
	if st := e.R.WSOpen(id, httptest.NewRequest("GET", "/api/v2/ws", nil)); st != 0 {
		e.T.Fatalf("WSOpen = %d", st)
	}
	e.T.Cleanup(func() { e.R.WSClose(id) })
	return &WS{e: e, ID: id}
}

// Send sends one text frame.
func (w *WS) Send(frame string) { w.e.R.WSMessage(w.ID, []byte(frame)) }

// Next returns the next frame for which match is true, skipping others; it
// fails the test if none comes within timeout.
func (w *WS) Next(match func(map[string]any) bool, timeout time.Duration) map[string]any {
	w.e.T.Helper()
	if m := w.next(match, timeout); m != nil {
		return m
	}
	w.e.T.Fatalf("no matching hermes.v1 frame within %v; got %s", timeout, w.e.Radio.Frames(w.ID))
	return nil
}

// None fails the test if a frame for which match is true arrives within d.
func (w *WS) None(match func(map[string]any) bool, d time.Duration) {
	w.e.T.Helper()
	if m := w.next(match, d); m != nil {
		w.e.T.Fatalf("unexpected frame %v", m)
	}
}

func (w *WS) next(match func(map[string]any) bool, timeout time.Duration) map[string]any {
	deadline := time.Now().Add(timeout)
	for {
		frames := w.e.Radio.Frames(w.ID)
		for ; w.read < len(frames); w.read++ {
			var m map[string]any
			if json.Unmarshal(frames[w.read], &m) == nil && match(m) {
				w.read++
				return m
			}
		}
		if time.Now().After(deadline) {
			return nil
		}
		time.Sleep(5 * time.Millisecond)
	}
}

// Type matches frames of one type.
func Type(t string) func(map[string]any) bool {
	return func(m map[string]any) bool { return m["type"] == t }
}

// Cmd runs one hermes.v1 command on a fresh connection and returns its
// reply frame.
func (e *Env) Cmd(name, args string) map[string]any {
	e.T.Helper()
	w := e.Open()
	frame := `{"type":"cmd","id":"t1","cmd":"` + name + `"`
	if args != "" {
		frame += `,"args":` + args
	}
	w.Send(frame + "}")
	return w.Next(Type("reply"), 2*time.Second)
}
