package station_test

import (
	"context"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/setup"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/station"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
)

// TestAuthenticatedRouterSetupCodePath guards the wiring between
// NewAuthenticatedRouter's setup.Config and the setup handler: first-boot
// setup must read the one-time code from cfg.SetupCodePath, not the default
// /etc/hermes/setup-code. The integration tests hit this through
// SETUP_CODE_PATH (config.Load).
func TestAuthenticatedRouterSetupCodePath(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "setup-code")
	if err := os.WriteFile(path, []byte("abc123\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	st, err := store.Open(context.Background(), filepath.Join(dir, "h.sqlite"))
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	t.Cleanup(func() { st.Close() })

	rt := station.NewAuthenticatedRouter(rad.NewStub(), st, setup.Config{SetupCodePath: path})

	body := `{"setup_code":"abc123","station_name":"Test","language":"en","timezone":"UTC",` +
		`"admin":{"username":"alice","password":"password1"}}`
	req := httptest.NewRequest("POST", "/api/v2/setup", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()
	rt.ServeHTTP(w, req)
	if w.Code != 201 {
		t.Fatalf("POST /setup = %d: %s", w.Code, w.Body)
	}
}
