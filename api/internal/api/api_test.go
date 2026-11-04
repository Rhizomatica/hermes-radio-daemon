package api_test

import (
	"context"
	"encoding/json/jsontext"
	"net/http"
	"net/http/httptest"
	"reflect"
	"strings"
	"testing"
	"time"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
	"github.com/Rhizomatica/hermes-radio-daemon/api/spec"
)

// Every registered operation and command is in the contract (Op and
// Command panic otherwise), and none is registered twice.
func TestRegistration(t *testing.T) {
	e := testapi.New(t)
	for _, id := range e.R.Operations() {
		if _, err := spec.Op(id); err != nil {
			t.Error(err)
		}
	}
	if got := e.R.Commands(spec.RoleAdmin); len(got) == 0 {
		t.Error("no hermes.v1 command registered")
	}
	defer func() {
		if recover() == nil {
			t.Error("registering an operation the spec does not have did not panic")
		}
	}()
	e.R.Op("noSuchOperation", func(http.ResponseWriter, *http.Request) {})
}

func TestNotFoundAndMethodNotAllowed(t *testing.T) {
	e := testapi.New(t)
	r := e.Do("GET", "/nothing/here", "")
	if r.Status != 404 || r.Header.Get("Content-Type") != "application/problem+json" {
		t.Fatalf("unknown path: %d %s", r.Status, r.Header.Get("Content-Type"))
	}
	if j := r.JSON(t); j["code"] != "not_found" || j["request_id"] == "" {
		t.Errorf("problem = %v", j)
	}
	r = e.Do("DELETE", "/radio", "")
	if r.Status != 405 || r.Header.Get("Allow") != "GET" {
		t.Errorf("DELETE /radio: %d, Allow %q", r.Status, r.Header.Get("Allow"))
	}
}

func TestRequestID(t *testing.T) {
	e := testapi.New(t)
	req := httptest.NewRequest("GET", "/api/v2/health", nil)
	req.Header.Set("X-Request-Id", "abc-123")
	w := httptest.NewRecorder()
	e.R.ServeHTTP(w, req)
	if got := w.Header().Get("X-Request-Id"); got != "abc-123" {
		t.Errorf("a client's id is echoed: got %q", got)
	}
	req.Header.Set("X-Request-Id", strings.Repeat("x", 65))
	w = httptest.NewRecorder()
	e.R.ServeHTTP(w, req)
	if got := w.Header().Get("X-Request-Id"); len(got) != 16 {
		t.Errorf("an over-long id is replaced: got %q", got)
	}
}

// Roles are enforced per the spec's x-hermes-role; public operations need none.
func TestRoles(t *testing.T) {
	e := testapi.New(t)
	e.R.RoleOf = func(*http.Request) spec.Role { return spec.RoleReadonly }
	if r := e.Do("GET", "/radio", ""); r.Status != 200 {
		t.Errorf("readonly GET /radio = %d", r.Status)
	}
	if r := e.Do("POST", "/radio/protection/reset", ""); r.Status != 403 {
		t.Errorf("readonly resetProtection = %d, want 403", r.Status)
	}
	e.R.RoleOf = func(*http.Request) spec.Role { return spec.RoleNone }
	if r := e.Do("GET", "/health", ""); r.Status != 200 {
		t.Errorf("getHealth with no session = %d, want 200 (public)", r.Status)
	}
	if !api.AtLeast(spec.RoleAdmin, spec.RoleOperator) || api.AtLeast(spec.RoleUser, spec.RoleOperator) ||
		api.AtLeast(spec.RoleNone, spec.RoleReadonly) {
		t.Error("role order")
	}
}

func TestDecodeErrors(t *testing.T) {
	e := testapi.New(t)
	req := httptest.NewRequest("PATCH", "/api/v2/radio/profiles/0", strings.NewReader(`{"power":5}`))
	w := httptest.NewRecorder()
	e.R.ServeHTTP(w, req) // no Content-Type
	if w.Code != 415 {
		t.Errorf("no Content-Type = %d, want 415", w.Code)
	}
	if r := e.Do("PUT", "/radio/active-profile", `{"profile":1,"extra":2}`); r.Status != 400 ||
		r.JSON(t)["detail"] != "extra: unknown field" {
		t.Errorf("unknown field = %d %s, want 400", r.Status, r.Body)
	}
	// encoding/json/v2 reads strictly: what encoding/json let through, and
	// a later reader could read another way, is a 400. The detail is the
	// API's wording, not the library's.
	for _, c := range []struct{ method, path, body, detail string }{
		{"PUT", "/radio/active-profile", `{"Profile":1}`, "Profile: unknown field"},
		{"PUT", "/radio/active-profile", `{"profile":1,"profile":2}`, "profile: duplicate field"},
		{"PATCH", "/radio/profiles/0", `{"power":5,"power":90}`, "power: duplicate field"},
		{"PUT", "/radio/active-profile", `{"profile":1} {"profile":2}`, "malformed JSON at byte 14"},
		{"PATCH", "/radio/profiles/0", "{\"mode\":\"US\xffB\"}", "mode: invalid UTF-8"},
		{"PATCH", "/radio/profiles/0", `{"power":`, "malformed JSON: it ends early"},
		{"PUT", "/radio/active-profile", `[1]`, "expected a JSON object"},
		{"PUT", "/radio/active-profile", `{"profile":"1"}`, "profile: expected int"},
	} {
		if r := e.Do(c.method, c.path, c.body); r.Status != 400 || r.JSON(t)["detail"] != c.detail {
			t.Errorf("%s %s %q = %d %s, want 400 %q", c.method, c.path, c.body, r.Status, r.Body, c.detail)
		}
	}
	if e.Radio.State().ActiveProfile != 0 {
		t.Error("a refused body changed the active profile")
	}
}

// ServeRaw is what radiod's C server calls: parts in, parts out.
func TestServeRaw(t *testing.T) {
	e := testapi.New(t)
	st, ct, extra, body := e.R.ServeRaw("PATCH", "/api/v2/radio/profiles/1", "",
		"Host: station\r\nContent-Type: application/json\r\nX-Request-Id: r1\r\n", "127.0.0.1",
		[]byte(`{"frequency_hz":14070000}`))
	if st != 200 || ct != "application/json" || !strings.Contains(extra, "X-Request-Id: r1\r\n") ||
		!strings.Contains(string(body), `"frequency_hz":14070000`) {
		t.Errorf("ServeRaw = %d %q %q %s", st, ct, extra, body)
	}
	st, _, _, _ = e.R.ServeRaw("GET", "/api/v2/radio/digi", "limit=x", "", "127.0.0.1", nil)
	if st != 200 {
		t.Errorf("GET with a query = %d", st)
	}
}

// ServeRaw keeps the loopback-only policy while there are no sessions: a
// remote peer is refused, a loopback one is answered.
func TestServeRawLoopbackOnly(t *testing.T) {
	e := testapi.New(t)
	st, ct, _, _ := e.R.ServeRaw("GET", "/api/v2/health", "", "", "10.0.0.1", nil)
	if st != 403 || ct != "application/problem+json" {
		t.Errorf("remote peer without sessions = %d %q, want 403 problem+json", st, ct)
	}
	if st, _, _, _ = e.R.ServeRaw("GET", "/api/v2/health", "", "", "127.0.0.1", nil); st != 200 {
		t.Errorf("loopback peer without sessions = %d, want 200", st)
	}
}

// Which peers count as this machine for the no-session router.
func TestServeRawLoopbackPeers(t *testing.T) {
	e := testapi.New(t)
	for peer, want := range map[string]int{
		"127.0.0.1": 200, "127.0.0.1:1234": 200, "::1": 200, "[::1]:1234": 200,
		"::ffff:127.0.0.1": 200, "127.8.9.1": 200,
		"10.0.0.1": 403, "192.0.2.1:80": 403, "::ffff:10.0.0.1": 403, "2001:db8::1": 403,
		"": 403, "garbage": 403,
	} {
		if st, _, _, _ := e.R.ServeRaw("GET", "/api/v2/health", "", "", peer, nil); st != want {
			t.Errorf("peer %q = %d, want %d", peer, st, want)
		}
	}
}

// hermes.v1: hello on open, unknown commands and subscriptions answered.
func TestHermesV1Basics(t *testing.T) {
	e := testapi.New(t)
	req := httptest.NewRequest("GET", "/api/v2/ws", nil)
	req.RemoteAddr = "127.0.0.1" // the no-session router answers only on loopback
	if st := e.R.WSOpen(7, req); st != 0 {
		t.Fatalf("WSOpen = %d", st)
	}
	defer e.R.WSClose(7)
	frames := e.Radio.Frames(7)
	if len(frames) == 0 || !strings.Contains(string(frames[0]), `"type":"hello","protocol":"hermes.v1"`) {
		t.Fatalf("no hello: %q", frames)
	}
	if r := e.Cmd("radio.nonsense", ""); r["ok"] != false || r["error"].(map[string]any)["code"] != "unknown_cmd" {
		t.Errorf("unknown command: %v", r)
	}
	// a router where no domain registered the spec's commands
	bare := bareEnv(t)
	if r := bare.Cmd("rtc.offer", "{}"); r["error"].(map[string]any)["code"] != "unknown_cmd" {
		t.Errorf("a spec command with no handler is unknown_cmd: %v", r)
	}
}

// bareEnv is a router over a Stub with nothing registered.
func bareEnv(t *testing.T) *testapi.Env {
	stub := rad.NewStub()
	return &testapi.Env{T: t, Radio: stub, R: api.NewRouter(stub)}
}

// A refused frame's reply carries its id when the frame has exactly one
// top-level "id", so a client with several commands in flight knows which
// one failed.
func TestFrameErrorID(t *testing.T) {
	e := testapi.New(t)
	w := e.Open()
	w.Next(testapi.Type("hello"), time.Second)
	for _, c := range []struct{ frame, id string }{
		{`{"type":"cmd","id":"a","cmd":"radio.ptt","args":{"on":true,"on":false}}`, "a"},
		{"{\"type\":\"cmd\",\"cmd\":\"radio.ptt\",\"args\":{\"x\":\"\xff\"},\"id\":\"b\"}", "b"},
		{`{"id":"c","id":"d","type":"cmd","cmd":"radio.ptt"}`, ""}, // which one?
		{`{"type":"cmd","cmd":"radio.ptt","args":{"id":"e","id":"e"}}`, ""},
		{`{"type":"cmd","id":7,"cmd":"radio.ptt","args":{"on":1,"on":2}}`, ""},
		{`{"type":"cmd","id":"f","cmd":"radio.ptt","args":{"on":`, ""},
		{`["id","g"]`, ""},
	} {
		w.Send(c.frame)
		r := w.Next(testapi.Type("reply"), time.Second)
		if er, _ := r["error"].(map[string]any); r["id"] != c.id || er["code"] != "bad_request" {
			t.Errorf("%q: reply %v, want bad_request with id %q", c.frame, r, c.id)
		}
	}
}

// A command's result is the reply's data even when it is empty: {} and []
// are values, not a missing member (encoding/json/v2's omitempty would
// drop them).
func TestReplyEmptyData(t *testing.T) {
	e := bareEnv(t)
	e.R.Command("rtc.offer", spec.RoleUser, func(context.Context, *api.Conn, jsontext.Value) (any, *api.CmdError) {
		return struct{}{}, nil
	})
	e.R.Command("rtc.close", spec.RoleUser, func(context.Context, *api.Conn, jsontext.Value) (any, *api.CmdError) {
		return []any{}, nil
	})
	for name, want := range map[string]any{"rtc.offer": map[string]any{}, "rtc.close": []any{}} {
		r := e.Cmd(name, "{}")
		if d, ok := r["data"]; r["ok"] != true || !ok || !reflect.DeepEqual(d, want) {
			t.Errorf("%s: reply %v, want data %v", name, r, want)
		}
		if _, ok := r["error"]; ok {
			t.Errorf("%s: an ok reply has an error member: %v", name, r)
		}
	}
}
