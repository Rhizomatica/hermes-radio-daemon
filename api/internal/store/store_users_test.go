package store

import (
	"context"
	"database/sql"
	"errors"
	"path/filepath"
	"testing"
)

func openStore(t *testing.T) *Store {
	t.Helper()
	s, err := Open(context.Background(), filepath.Join(t.TempDir(), "hermes.sqlite"))
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	t.Cleanup(func() { s.Close() })
	return s
}

func strptr(s string) *string { return &s }

func TestUsersCRUD(t *testing.T) {
	ctx := context.Background()
	s := openStore(t)

	dn := "Alice Admin"
	u, err := s.CreateUser(ctx, NewUser{
		Username: "alice", PasswordHash: "h1", Role: "admin",
		DisplayName: &dn, Locale: strptr("en"),
	})
	if err != nil {
		t.Fatalf("CreateUser: %v", err)
	}
	if u.ID == 0 || u.Username != "alice" || u.Role != "admin" || u.Disabled {
		t.Fatalf("created user = %+v", u)
	}
	if u.DisplayName == nil || *u.DisplayName != "Alice Admin" {
		t.Fatalf("display_name = %v", u.DisplayName)
	}

	// Unique username.
	if _, err := s.CreateUser(ctx, NewUser{Username: "alice", PasswordHash: "x", Role: "user"}); err == nil {
		t.Fatal("duplicate username: expected error")
	}

	// Fetch by username and id.
	got, err := s.UserByUsername(ctx, "alice")
	if err != nil || got.ID != u.ID {
		t.Fatalf("UserByUsername = %+v, %v", got, err)
	}
	got, err = s.UserByID(ctx, u.ID)
	if err != nil || got.Username != "alice" {
		t.Fatalf("UserByID = %+v, %v", got, err)
	}

	// Update: change role, set phone, then clear it.
	role := "operator"
	phone := "+1 555"
	u2, err := s.UpdateUser(ctx, u.ID, UserPatch{Role: &role, Phone: &phone})
	if err != nil || u2.Role != "operator" || u2.Phone == nil || *u2.Phone != "+1 555" {
		t.Fatalf("UpdateUser role/phone = %+v, %v", u2, err)
	}
	u2, err = s.UpdateUser(ctx, u.ID, UserPatch{ClearPhone: true})
	if err != nil || u2.Phone != nil {
		t.Fatalf("UpdateUser ClearPhone = %+v, %v", u2, err)
	}
	if _, err := s.UpdateUser(ctx, 999, UserPatch{Role: &role}); !errors.Is(err, sql.ErrNoRows) {
		t.Fatalf("UpdateUser missing = %v, want ErrNoRows", err)
	}

	// ListUsers pages by id.
	if _, err := s.CreateUser(ctx, NewUser{Username: "bob", PasswordHash: "h2", Role: "user"}); err != nil {
		t.Fatalf("CreateUser bob: %v", err)
	}
	if _, err := s.CreateUser(ctx, NewUser{Username: "carol", PasswordHash: "h3", Role: "readonly"}); err != nil {
		t.Fatalf("CreateUser carol: %v", err)
	}
	page, next, err := s.ListUsers(ctx, 2, 0)
	if err != nil || len(page) != 2 || next == 0 {
		t.Fatalf("ListUsers page1 = %d rows, next %d, err %v", len(page), next, err)
	}
	page2, next2, err := s.ListUsers(ctx, 2, next)
	if err != nil || len(page2) != 1 || next2 != 0 {
		t.Fatalf("ListUsers page2 = %d rows, next %d, err %v", len(page2), next2, err)
	}

	// Password hash + last login.
	if err := s.SetPasswordHash(ctx, u.ID, "newhash"); err != nil {
		t.Fatalf("SetPasswordHash: %v", err)
	}
	if err := s.SetLastLoginAt(ctx, u.ID); err != nil {
		t.Fatalf("SetLastLoginAt: %v", err)
	}
	got, _ = s.UserByID(ctx, u.ID)
	if got.PasswordHash != "newhash" || got.LastLoginAt == nil {
		t.Fatalf("after SetPasswordHash/SetLastLoginAt = %+v", got)
	}

	// CountUsers.
	if n, err := s.CountUsers(ctx); err != nil || n != 3 {
		t.Fatalf("CountUsers = %d, %v", n, err)
	}

	// Delete bob; cascade removes bob's sessions too.
	if err := s.DeleteUser(ctx, 2); err != nil {
		t.Fatalf("DeleteUser bob: %v", err)
	}
	if err := s.DeleteUser(ctx, 999); !errors.Is(err, sql.ErrNoRows) {
		t.Fatalf("DeleteUser missing = %v, want ErrNoRows", err)
	}
}

func TestCreateUserDuplicateIsTyped(t *testing.T) {
	ctx := context.Background()
	s := openStore(t)
	if _, err := s.CreateUser(ctx, NewUser{Username: "dup", PasswordHash: "h", Role: "user"}); err != nil {
		t.Fatalf("CreateUser: %v", err)
	}
	_, err := s.CreateUser(ctx, NewUser{Username: "dup", PasswordHash: "h", Role: "user"})
	if !errors.Is(err, ErrUsernameTaken) {
		t.Fatalf("duplicate CreateUser = %v, want ErrUsernameTaken", err)
	}
}
