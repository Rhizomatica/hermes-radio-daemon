package api_test

import (
	"net/http/httptest"
	"testing"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
)

func corsReq(e *testapi.Env, method, path, origin string, hdr map[string]string) *httptest.ResponseRecorder {
	req := httptest.NewRequest(method, "/api/v2"+path, nil)
	req.RemoteAddr = "192.0.2.10"
	if origin != "" {
		req.Header.Set("Origin", origin)
	}
	for k, v := range hdr {
		req.Header.Set(k, v)
	}
	w := httptest.NewRecorder()
	e.R.ServeHTTP(w, req)
	return w
}

var preflight = map[string]string{
	"Access-Control-Request-Method":  "PATCH",
	"Access-Control-Request-Headers": "authorization, content-type, x-custom",
}

// The default, any origin: a preflight is answered before the role gate, with
// the headers it asked for, and never with credentials.
func TestCORSAnyOrigin(t *testing.T) {
	e := testapi.NewAuth(t)
	e.R.SetCORSOrigins("*")

	w := corsReq(e, "OPTIONS", "/radio/profiles/0", "https://app.example.org", preflight)
	h := w.Header()
	if w.Code != 204 || h.Get("Access-Control-Allow-Origin") != "*" ||
		h.Get("Access-Control-Allow-Headers") != "authorization, content-type, x-custom" ||
		h.Get("Access-Control-Allow-Methods") == "" {
		t.Fatalf("preflight = %d %v", w.Code, h)
	}
	if h.Get("Access-Control-Allow-Credentials") != "" {
		t.Fatal("credentials allowed cross-origin")
	}

	// The request itself still meets the role gate.
	w = corsReq(e, "GET", "/radio", "https://app.example.org", nil)
	if w.Code != 401 || w.Header().Get("Access-Control-Allow-Origin") != "*" ||
		w.Header().Get("Access-Control-Expose-Headers") == "" {
		t.Fatalf("GET without a session = %d %v", w.Code, w.Header())
	}
	// No Origin, no CORS headers.
	if w = corsReq(e, "GET", "/health", "", nil); w.Header().Get("Access-Control-Allow-Origin") != "" {
		t.Fatal("CORS headers without an Origin")
	}
}

// A list allows only its origins, echoed back as sent.
func TestCORSList(t *testing.T) {
	e := testapi.NewAuth(t)
	e.R.SetCORSOrigins("https://App.Example.org/, http://192.168.10.5:3000 ftp://nope *.bad")

	w := corsReq(e, "OPTIONS", "/radio", "https://app.example.org", preflight)
	if w.Code != 204 || w.Header().Get("Access-Control-Allow-Origin") != "https://app.example.org" {
		t.Fatalf("listed origin preflight = %d %v", w.Code, w.Header())
	}
	if w = corsReq(e, "GET", "/health", "http://192.168.10.5:3000", nil); w.Header().Get("Access-Control-Allow-Origin") != "http://192.168.10.5:3000" {
		t.Fatalf("listed origin GET: %v", w.Header())
	}
	for _, o := range []string{"https://evil.example", "ftp://nope", "null"} {
		w = corsReq(e, "OPTIONS", "/radio", o, preflight)
		if w.Header().Get("Access-Control-Allow-Origin") != "" || w.Code == 204 {
			t.Errorf("origin %q allowed: %d %v", o, w.Code, w.Header())
		}
	}
}

// Empty turns CORS off; and the no-session router (every loopback caller
// admin) never sends CORS headers, whatever is configured.
func TestCORSOff(t *testing.T) {
	e := testapi.NewAuth(t)
	e.R.SetCORSOrigins("")
	if w := corsReq(e, "GET", "/health", "https://app.example.org", nil); w.Header().Get("Access-Control-Allow-Origin") != "" {
		t.Fatal("CORS headers with CORS off")
	}
	n := testapi.New(t)
	n.R.SetCORSOrigins("*")
	req := httptest.NewRequest("GET", "/api/v2/health", nil)
	req.Header.Set("Origin", "https://app.example.org")
	w := httptest.NewRecorder()
	n.R.ServeHTTP(w, req)
	if w.Header().Get("Access-Control-Allow-Origin") != "" {
		t.Fatal("the no-session router sent CORS headers")
	}
}

func TestWSToken(t *testing.T) {
	req := httptest.NewRequest("GET", "/api/v2/ws", nil)
	req.Header.Set("Sec-WebSocket-Protocol", "hermes.v1, hermes.token.abc_DEF-123")
	if got := api.WSToken(req); got != "abc_DEF-123" {
		t.Fatalf("WSToken = %q", got)
	}
}
