// Package testapi helps test API handlers: a router over a radio Stub, and
// requests and hermes.v1 commands against it, with no C and no radiod.
package testapi

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
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

// Cmd runs one hermes.v1 command on a fresh connection and returns its
// reply frame.
func (e *Env) Cmd(name, args string) map[string]any {
	e.T.Helper()
	conn := time.Now().UnixNano()
	if st := e.R.WSOpen(conn, httptest.NewRequest("GET", "/api/v2/ws", nil)); st != 0 {
		e.T.Fatalf("WSOpen = %d", st)
	}
	defer e.R.WSClose(conn)
	frame := `{"type":"cmd","id":"t1","cmd":"` + name + `"`
	if args != "" {
		frame += `,"args":` + args
	}
	e.R.WSMessage(conn, []byte(frame+"}"))
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		for _, f := range e.Radio.Frames(conn) {
			var m map[string]any
			if json.Unmarshal(f, &m) == nil && m["type"] == "reply" {
				return m
			}
		}
		time.Sleep(5 * time.Millisecond)
	}
	e.T.Fatalf("no reply to %s", name)
	return nil
}
