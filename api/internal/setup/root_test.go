package setup

import (
	"context"
	"path/filepath"
	"testing"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/passwd"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
)

func openStore(t *testing.T) *store.Store {
	t.Helper()
	st, err := store.Open(context.Background(), filepath.Join(t.TempDir(), "h.sqlite"))
	if err != nil {
		t.Fatalf("store.Open: %v", err)
	}
	t.Cleanup(func() { st.Close() })
	return st
}

func TestEnsureRootSeedsFreshStation(t *testing.T) {
	ctx := context.Background()
	st := openStore(t)

	made, err := EnsureRoot(ctx, st)
	if err != nil || !made {
		t.Fatalf("first start: made=%v err=%v, want root", made, err)
	}
	u, err := st.UserByUsername(ctx, store.RootUsername)
	if err != nil {
		t.Fatalf("UserByUsername: %v", err)
	}
	if u.Role != "admin" || !passwd.Verify(u.PasswordHash, "caduceu") {
		t.Fatalf("root: role %q, password ok %v", u.Role, passwd.Verify(u.PasswordHash, "caduceu"))
	}
	if v, ok, _ := st.GetMeta(ctx, "setup.done"); !ok || v != "1" {
		t.Fatalf("setup.done = %q, %v; want 1", v, ok)
	}

	// The next start finds root and changes nothing.
	made, err = EnsureRoot(ctx, st)
	if err != nil || made {
		t.Fatalf("second start: made=%v err=%v, want nothing", made, err)
	}
	if n, _ := st.CountUsers(ctx); n != 1 {
		t.Fatalf("users = %d, want 1", n)
	}
}

func TestEnsureRootSeedsExistingStation(t *testing.T) {
	ctx := context.Background()
	st := openStore(t)
	if _, err := st.CreateUser(ctx, store.NewUser{Username: "alice", PasswordHash: "h", Role: "admin"}); err != nil {
		t.Fatalf("CreateUser: %v", err)
	}

	made, err := EnsureRoot(ctx, st)
	if err != nil || !made {
		t.Fatalf("existing station: made=%v err=%v, want root", made, err)
	}
	if _, err := st.UserByUsername(ctx, store.RootUsername); err != nil {
		t.Fatalf("root missing: %v", err)
	}
	if n, _ := st.CountUsers(ctx); n != 2 {
		t.Fatalf("users = %d, want 2", n)
	}
}

func TestEnsureRootReseedsAfterDeletion(t *testing.T) {
	ctx := context.Background()
	st := openStore(t)
	if _, err := EnsureRoot(ctx, st); err != nil {
		t.Fatalf("EnsureRoot: %v", err)
	}
	u, err := st.UserByUsername(ctx, store.RootUsername)
	if err != nil {
		t.Fatalf("UserByUsername: %v", err)
	}
	if err := st.DeleteUser(ctx, u.ID); err != nil {
		t.Fatalf("DeleteUser: %v", err)
	}

	made, err := EnsureRoot(ctx, st)
	if err != nil || !made {
		t.Fatalf("after deletion: made=%v err=%v, want root reseeded", made, err)
	}
	if _, err := st.UserByUsername(ctx, store.RootUsername); err != nil {
		t.Fatalf("root not reseeded: %v", err)
	}
}
