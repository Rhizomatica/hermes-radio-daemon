package auth_test

import (
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"testing"
)

func TestCSRF(t *testing.T) {
	rt, st := newEnv(t)
	id := seedUser(t, st, "alice", "password1", "user")

	// Log in to obtain the session cookie.
	req := httptest.NewRequest("POST", "/api/v2/session", strings.NewReader(`{"username":"alice","password":"password1"}`))
	req.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()
	rt.ServeHTTP(w, req)
	if w.Code != 201 {
		t.Fatalf("login = %d", w.Code)
	}
	var sc *http.Cookie
	for _, c := range w.Result().Cookies() {
		if c.Name == "hermes_session" {
			sc = c
		}
	}
	if sc == nil {
		t.Fatal("no hermes_session cookie")
	}

	patch := `{"display_name":"Alice"}`
	path := "/api/v2/users/" + strconv.FormatInt(id, 10)

	// A mutating request with the cookie and no Origin is refused.
	req = httptest.NewRequest("PATCH", path, strings.NewReader(patch))
	req.Header.Set("Content-Type", "application/json")
	req.AddCookie(sc)
	w = httptest.NewRecorder()
	rt.ServeHTTP(w, req)
	if w.Code != 403 {
		t.Fatalf("cookie PATCH without Origin = %d, want 403 csrf", w.Code)
	}

	// X-Hermes-CSRF: 1 satisfies the check.
	req = httptest.NewRequest("PATCH", path, strings.NewReader(patch))
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("X-Hermes-CSRF", "1")
	req.AddCookie(sc)
	w = httptest.NewRecorder()
	rt.ServeHTTP(w, req)
	if w.Code != 200 {
		t.Fatalf("cookie PATCH with X-Hermes-CSRF = %d, want 200", w.Code)
	}

	// A same-origin Origin header satisfies it too.
	req = httptest.NewRequest("PATCH", path, strings.NewReader(patch))
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Origin", "http://"+req.Host)
	req.AddCookie(sc)
	w = httptest.NewRecorder()
	rt.ServeHTTP(w, req)
	if w.Code != 200 {
		t.Fatalf("cookie PATCH with Origin = %d, want 200", w.Code)
	}

	// A Bearer token is exempt, even without Origin.
	w = do(rt, "POST", "/api/v2/session", `{"username":"alice","password":"password1"}`, "")
	token := jsonBody(t, w)["token"].(string)
	if w := do(rt, "PATCH", path, patch, token); w.Code != 200 {
		t.Fatalf("bearer PATCH = %d, want 200 (exempt)", w.Code)
	}
}

func TestWSOrigin(t *testing.T) {
	rt, st := newEnv(t)
	seedUser(t, st, "alice", "password1", "user")

	w := do(rt, "POST", "/api/v2/session", `{"username":"alice","password":"password1"}`, "")
	token := jsonBody(t, w)["token"].(string)
	var sc *http.Cookie
	for _, c := range w.Result().Cookies() {
		if c.Name == "hermes_session" {
			sc = c
		}
	}
	if sc == nil {
		t.Fatal("no hermes_session cookie")
	}

	// A cookie upgrade without an Origin is refused.
	req := httptest.NewRequest("GET", "/api/v2/ws", nil)
	req.AddCookie(sc)
	if code := rt.WSOpen(101, req); code != 403 {
		t.Fatalf("cookie WS without Origin = %d, want 403", code)
	}
	// With an Origin naming this station it is accepted.
	req = httptest.NewRequest("GET", "/api/v2/ws", nil)
	req.Header.Set("Origin", "http://"+req.Host)
	req.AddCookie(sc)
	if code := rt.WSOpen(102, req); code != 0 {
		t.Fatalf("cookie WS with Origin = %d, want 0", code)
	}
	defer rt.WSClose(102)
	// A Bearer token is exempt.
	req = httptest.NewRequest("GET", "/api/v2/ws", nil)
	req.Header.Set("Authorization", "Bearer "+token)
	if code := rt.WSOpen(103, req); code != 0 {
		t.Fatalf("bearer WS = %d, want 0", code)
	}
	defer rt.WSClose(103)
}
