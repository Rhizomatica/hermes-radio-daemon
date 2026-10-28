package setup_test

import (
	"context"
	"encoding/json"
	"io"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/auth"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/setup"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
)

func newRouter(t *testing.T, code string) *api.Router {
	rt, _ := newRouterPath(t, code)
	return rt
}

func newRouterPath(t *testing.T, code string) (*api.Router, string) {
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
	return rt, path
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

// The one-time code is retired once setup has used it.
func TestSetupRetiresCode(t *testing.T) {
	rt, path := newRouterPath(t, "abc123")
	if w := do(rt, "POST", "/setup", body); w.Code != 201 {
		t.Fatalf("runSetup = %d: %s", w.Code, w.Body)
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Fatalf("setup code file still there: %v", err)
	}
}

// A failed setup (wrong code) leaves the code in place.
func TestSetupKeepsCodeOnFailure(t *testing.T) {
	rt, path := newRouterPath(t, "abc123")
	do(rt, "POST", "/setup", strings.Replace(body, "abc123", "wrong", 1))
	if _, err := os.Stat(path); err != nil {
		t.Fatalf("setup code file gone after a failed setup: %v", err)
	}
}

// Concurrent setups with the right code and different admins: exactly one wins.
func TestSetupConcurrent(t *testing.T) {
	rt := newRouter(t, "abc123")
	var wg sync.WaitGroup
	var mu sync.Mutex
	codes := map[int]int{}
	for i := 0; i < 6; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			b := strings.Replace(body, `"alice"`, `"admin`+string(rune('a'+i))+`"`, 1)
			w := do(rt, "POST", "/setup", b)
			mu.Lock()
			codes[w.Code]++
			mu.Unlock()
		}(i)
	}
	wg.Wait()
	if codes[201] != 1 || codes[409] != 5 {
		t.Fatalf("status codes = %v, want one 201 and five 409", codes)
	}
}
