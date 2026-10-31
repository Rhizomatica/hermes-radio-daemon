package api_test

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

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
	if r := e.Do("PATCH", "/radio/profiles/0", `{"power":`); r.Status != 400 {
		t.Errorf("broken JSON = %d, want 400", r.Status)
	}
	if r := e.Do("PUT", "/radio/active-profile", `{"profile":1,"extra":2}`); r.Status != 400 {
		t.Errorf("unknown field = %d, want 400", r.Status)
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
	if r := e.Cmd("rtc.offer", "{}"); r["error"].(map[string]any)["code"] != "unknown_cmd" {
		t.Errorf("a spec command not implemented yet is unknown_cmd: %v", r)
	}
}
