package setup_test

import (
	"context"
	"encoding/json"
	"io"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/auth"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/setup"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
)

func newRouter(t *testing.T, code string) *api.Router {
	t.Helper()
	st, err := store.Open(context.Background(), filepath.Join(t.TempDir(), "h.sqlite"))
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	t.Cleanup(func() { st.Close() })
	path := filepath.Join(t.TempDir(), "setup-code")
	if err := os.WriteFile(path, []byte(code), 0o600); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	rt := api.NewRouter(rad.NewStub())
	auth.Register(rt, st, auth.Config{})
	setup.Register(rt, st, setup.Config{SetupCodePath: path})
	return rt
}

func do(rt *api.Router, method, path, body string) *httptest.ResponseRecorder {
	var rd io.Reader
	if body != "" {
		rd = strings.NewReader(body)
	}
	req := httptest.NewRequest(method, "/api/v2"+path, rd)
	if body != "" {
		req.Header.Set("Content-Type", "application/json")
	}
	w := httptest.NewRecorder()
	rt.ServeHTTP(w, req)
	return w
}

func jb(t *testing.T, w *httptest.ResponseRecorder) map[string]any {
	t.Helper()
	var m map[string]any
	if err := json.Unmarshal(w.Body.Bytes(), &m); err != nil {
		t.Fatalf("body is not a JSON object: %v\n%s", err, w.Body.Bytes())
	}
	return m
}

const body = `{"setup_code":"abc123","station_name":"Test Station","callsign":"TEST1","country":"BR","language":"en","timezone":"America/Sao_Paulo","admin":{"username":"alice","password":"password1","display_name":"Alice"}}`

func TestSetupStatusAndRun(t *testing.T) {
	rt := newRouter(t, "abc123\n")

	if w := do(rt, "GET", "/setup", ""); w.Code != 200 || jb(t, w)["done"] != false {
		t.Fatalf("initial status = %d %s", w.Code, w.Body)
	}

	// Wrong code.
	if w := do(rt, "POST", "/setup", `{"setup_code":"nope","station_name":"x","language":"en","timezone":"UTC","admin":{"username":"a","password":"password1"}}`); w.Code != 403 {
		t.Fatalf("wrong code = %d, want 403", w.Code)
	}

	// Correct code creates the admin and returns a session.
	w := do(rt, "POST", "/setup", body)
	if w.Code != 201 {
		t.Fatalf("runSetup = %d: %s", w.Code, w.Body)
	}
	j := jb(t, w)
	if j["token"] == nil || j["token"] == "" {
		t.Fatalf("no token: %v", j)
	}
	if u := j["user"].(map[string]any); u["username"] != "alice" || u["role"] != "admin" {
		t.Fatalf("admin = %v", u)
	}

	// Done now.
	if w := do(rt, "GET", "/setup", ""); jb(t, w)["done"] != true {
		t.Fatalf("status after run = %s", w.Body)
	}
	// Running again is refused.
	if w := do(rt, "POST", "/setup", body); w.Code != 409 {
		t.Fatalf("second run = %d, want 409", w.Code)
	}
}

func TestSetupLockout(t *testing.T) {
	rt := newRouter(t, "abc123")

	wrong := `{"setup_code":"wrong","station_name":"x","language":"en","timezone":"UTC","admin":{"username":"a","password":"password1"}}`
	for i := 0; i < 5; i++ {
		if w := do(rt, "POST", "/setup", wrong); w.Code != 403 {
			t.Fatalf("wrong code %d = %d", i+1, w.Code)
		}
	}
	if w := do(rt, "POST", "/setup", wrong); w.Code != 429 {
		t.Fatalf("sixth wrong code = %d, want 429", w.Code)
	}
}
