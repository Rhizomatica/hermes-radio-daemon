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
	if u.Role != "admin" || !passwd.Verify(u.PasswordHash, rootDefaultPassword) {
		t.Fatalf("root: role %q, password ok %v", u.Role, passwd.Verify(u.PasswordHash, rootDefaultPassword))
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

func TestEnsureRootLeavesExistingStation(t *testing.T) {
	ctx := context.Background()
	st := openStore(t)
	if _, err := st.CreateUser(ctx, store.NewUser{Username: "alice", PasswordHash: "h", Role: "admin"}); err != nil {
		t.Fatalf("CreateUser: %v", err)
	}

	made, err := EnsureRoot(ctx, st)
	if err != nil || made {
		t.Fatalf("existing station: made=%v err=%v, want nothing", made, err)
	}
	if _, err := st.UserByUsername(ctx, store.RootUsername); err == nil {
		t.Fatal("root was added to a station that already had users")
	}
	if n, _ := st.CountUsers(ctx); n != 1 {
		t.Fatalf("users = %d, want 1", n)
	}
	// An imported or migrated database without setup.done is left alone:
	// no root appears and setup is not silently marked done.
	if _, ok, _ := st.GetMeta(ctx, "setup.done"); ok {
		t.Fatal("setup.done was set on an existing station")
	}
}

func TestEnsureRootDoesNotReseedAfterDeletion(t *testing.T) {
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
	if err != nil || made {
		t.Fatalf("after deletion: made=%v err=%v, want nothing", made, err)
	}
	if _, err := st.UserByUsername(ctx, store.RootUsername); err == nil {
		t.Fatal("root was reseeded after setup had completed")
	}
}
