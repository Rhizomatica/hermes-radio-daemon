package setup

import (
	"context"
	"path/filepath"
	"testing"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/passwd"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
)

func TestEnsureDefaultAdmin(t *testing.T) {
	ctx := context.Background()
	st, err := store.Open(ctx, filepath.Join(t.TempDir(), "h.sqlite"))
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	defer st.Close()

	made, err := EnsureDefaultAdmin(ctx, st)
	if err != nil || !made {
		t.Fatalf("first start: made=%v err=%v, want a default admin", made, err)
	}
	u, err := st.UserByUsername(ctx, DefaultAdminUsername)
	if err != nil {
		t.Fatalf("UserByUsername: %v", err)
	}
	if u.Role != "admin" || !passwd.Verify(u.PasswordHash, DefaultAdminPassword) {
		t.Fatalf("default admin: role %q, password ok %v", u.Role, passwd.Verify(u.PasswordHash, DefaultAdminPassword))
	}
	if v, ok, _ := st.GetMeta(ctx, "setup.done"); !ok || v != "1" {
		t.Fatalf("setup.done = %q, %v; want 1", v, ok)
	}

	// The next start finds a user and changes nothing.
	made, err = EnsureDefaultAdmin(ctx, st)
	if err != nil || made {
		t.Fatalf("second start: made=%v err=%v, want nothing", made, err)
	}
	if n, _ := st.CountUsers(ctx); n != 1 {
		t.Fatalf("users = %d, want 1", n)
	}

	// Setup done and every user deleted since: no admin appears behind the owner's back.
	if err := st.DeleteUser(ctx, u.ID); err != nil {
		t.Fatalf("DeleteUser: %v", err)
	}
	made, err = EnsureDefaultAdmin(ctx, st)
	if err != nil || made {
		t.Fatalf("after setup and deletion: made=%v err=%v, want nothing", made, err)
	}
}
