package auth_test

import (
	"context"
	"encoding/json"
	"io"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/auth"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/passwd"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/setup"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/station"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
)

func newEnv(t *testing.T) (*api.Router, *store.Store) {
	t.Helper()
	st, err := store.Open(context.Background(), filepath.Join(t.TempDir(), "h.sqlite"))
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	t.Cleanup(func() { st.Close() })
	return station.NewAuthenticatedRouter(rad.NewStub(), st, setup.Config{}), st
}

func seedUser(t *testing.T, st *store.Store, username, password, role string) int64 {
	t.Helper()
	h, err := passwd.Hash(password)
	if err != nil {
		t.Fatalf("passwd.Hash: %v", err)
	}
	u, err := st.CreateUser(context.Background(), store.NewUser{Username: username, PasswordHash: h, Role: role})
	if err != nil {
		t.Fatalf("CreateUser: %v", err)
	}
	return u.ID
}

func do(rt *api.Router, method, path, body, token string) *httptest.ResponseRecorder {
	var rd io.Reader
	if body != "" {
		rd = strings.NewReader(body)
	}
	req := httptest.NewRequest(method, path, rd)
	if body != "" {
		req.Header.Set("Content-Type", "application/json")
	}
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}
	w := httptest.NewRecorder()
	rt.ServeHTTP(w, req)
	return w
}

func jsonBody(t *testing.T, w *httptest.ResponseRecorder) map[string]any {
	t.Helper()
	var m map[string]any
	if err := json.Unmarshal(w.Body.Bytes(), &m); err != nil {
		t.Fatalf("body is not a JSON object: %v\n%s", err, w.Body.Bytes())
	}
	return m
}

func TestLoginGetAndLogout(t *testing.T) {
	rt, st := newEnv(t)
	seedUser(t, st, "alice", "correct horse", "admin")

	w := do(rt, "POST", "/api/v2/session", `{"username":"alice","password":"correct horse"}`, "")
	if w.Code != 201 {
		t.Fatalf("login = %d: %s", w.Code, w.Body.Bytes())
	}
	j := jsonBody(t, w)
	if j["token"] == nil || j["token"] == "" {
		t.Fatalf("login body has no token: %v", j)
	}
	token := j["token"].(string)
	if u := j["user"].(map[string]any); u["username"] != "alice" || u["role"] != "admin" || u["disabled"] != false {
		t.Fatalf("login user = %v", u)
	}
	cookie := w.Header().Get("Set-Cookie")
	if !strings.Contains(cookie, "hermes_session=") || !strings.Contains(cookie, "HttpOnly") || !strings.Contains(cookie, "SameSite=Strict") {
		t.Fatalf("Set-Cookie = %q", cookie)
	}

	w = do(rt, "GET", "/api/v2/session", "", token)
	if w.Code != 200 {
		t.Fatalf("getSession = %d: %s", w.Code, w.Body.Bytes())
	}
	j = jsonBody(t, w)
	if _, has := j["token"]; has {
		t.Fatalf("getSession returned a token: %v", j)
	}
	if j["user"].(map[string]any)["username"] != "alice" {
		t.Fatalf("getSession user = %v", j["user"])
	}

	if w := do(rt, "DELETE", "/api/v2/session", "", token); w.Code != 204 {
		t.Fatalf("logout = %d", w.Code)
	}
	if w := do(rt, "GET", "/api/v2/session", "", token); w.Code != 401 {
		t.Fatalf("getSession after logout = %d, want 401", w.Code)
	}
}

func TestLoginFailures(t *testing.T) {
	rt, st := newEnv(t)
	seedUser(t, st, "alice", "correct horse", "user")

	if w := do(rt, "POST", "/api/v2/session", `{"username":"alice","password":"wrong"}`, ""); w.Code != 401 {
		t.Fatalf("wrong password = %d", w.Code)
	}
	if w := do(rt, "POST", "/api/v2/session", `{"username":"nobody","password":"x"}`, ""); w.Code != 401 {
		t.Fatalf("unknown user = %d", w.Code)
	}
}

func TestLoginRateLimit(t *testing.T) {
	rt, st := newEnv(t)
	seedUser(t, st, "alice", "correct horse", "user")

	for i := 0; i < 5; i++ {
		if w := do(rt, "POST", "/api/v2/session", `{"username":"alice","password":"wrong"}`, ""); w.Code != 401 {
			t.Fatalf("failed login %d = %d", i+1, w.Code)
		}
	}
	w := do(rt, "POST", "/api/v2/session", `{"username":"alice","password":"wrong"}`, "")
	if w.Code != 429 {
		t.Fatalf("sixth failed login = %d, want 429", w.Code)
	}
	if j := jsonBody(t, w); j["code"] != "rate_limited" {
		t.Fatalf("code = %v", j["code"])
	}
	if w.Header().Get("Retry-After") == "" {
		t.Fatal("no Retry-After")
	}
}

// A successful login does not count against the failed-login limit.
func TestLoginSuccessIsNotCounted(t *testing.T) {
	rt, st := newEnv(t)
	seedUser(t, st, "alice", "correct horse", "user")

	for i := 0; i < 8; i++ {
		if w := do(rt, "POST", "/api/v2/session", `{"username":"alice","password":"correct horse"}`, ""); w.Code != 201 {
			t.Fatalf("login %d = %d, want 201", i+1, w.Code)
		}
	}
}

// Parallel guesses are counted before they are checked: a burst cannot get
// past the limit.
func TestLoginRateLimitParallel(t *testing.T) {
	rt, st := newEnv(t)
	seedUser(t, st, "alice", "correct horse", "user")

	var wg sync.WaitGroup
	var mu sync.Mutex
	codes := map[int]int{}
	for i := 0; i < 20; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			w := do(rt, "POST", "/api/v2/session", `{"username":"alice","password":"wrong"}`, "")
			mu.Lock()
			codes[w.Code]++
			mu.Unlock()
		}()
	}
	wg.Wait()
	if codes[401] != 5 || codes[429] != 15 {
		t.Fatalf("status codes = %v, want 5x401 and 15x429", codes)
	}
}

// A login sweeps expired sessions out of the table.
func TestLoginPurgesExpiredSessions(t *testing.T) {
	rt, st := newEnv(t)
	id := seedUser(t, st, "alice", "correct horse", "user")
	old, err := st.CreateSession(context.Background(), store.NewSession{
		UserID: id, TokenHash: "stale", ExpiresAt: time.Now().Add(-time.Hour).UTC().Format(time.RFC3339),
	})
	if err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	if w := do(rt, "POST", "/api/v2/session", `{"username":"alice","password":"correct horse"}`, ""); w.Code != 201 {
		t.Fatalf("login = %d", w.Code)
	}
	if _, err := st.SessionByID(context.Background(), old.ID); err == nil {
		t.Fatal("expired session was not purged")
	}
}

func TestRateKey(t *testing.T) {
	for peer, want := range map[string]string{
		"192.0.2.7:4000":              "192.0.2.7",
		"192.0.2.7":                   "192.0.2.7",
		"[::ffff:192.0.2.7]:4000":     "192.0.2.7",
		"[2001:db8:1:2::5]:4000":      "2001:db8:1:2::/64",
		"[2001:db8:1:2:ffff::9]:4000": "2001:db8:1:2::/64",
		"[2001:db8:1:3::5]:4000":      "2001:db8:1:3::/64",
		"garbage":                     "garbage",
	} {
		r := httptest.NewRequest("GET", "/", nil)
		r.RemoteAddr = peer
		if got := auth.RateKey(r); got != want {
			t.Errorf("RateKey(%q) = %q, want %q", peer, got, want)
		}
	}
}
