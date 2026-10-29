package store

import (
	"context"
	"database/sql"
	"errors"
	"testing"
	"time"
)

func TestSessions(t *testing.T) {
	ctx := context.Background()
	s := openStore(t)

	u, _ := s.CreateUser(ctx, NewUser{Username: "alice", PasswordHash: "h", Role: "user"})

	ss, err := s.CreateSession(ctx, NewSession{
		UserID: u.ID, TokenHash: "tok1", IP: "10.0.0.1", UserAgent: "test", ExpiresAt: "2026-12-01T00:00:00Z",
	})
	if err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	if ss.ID == 0 || ss.UserID != u.ID || ss.IP == nil || *ss.IP != "10.0.0.1" {
		t.Fatalf("session = %+v", ss)
	}

	// Auth join resolves the role and disabled flag.
	a, err := s.AuthSessionByTokenHash(ctx, "tok1")
	if err != nil || a.Role != "user" || a.Disabled || a.LastSeenAt == "" || a.ExpiresAt != "2026-12-01T00:00:00Z" || a.SessionID != ss.ID {
		t.Fatalf("AuthSessionByTokenHash = %+v, %v", a, err)
	}
	if _, err := s.AuthSessionByTokenHash(ctx, "nope"); !errors.Is(err, sql.ErrNoRows) {
		t.Fatalf("unknown token hash = %v, want ErrNoRows", err)
	}

	// Empty ip/user-agent are stored as NULL.
	ss2, _ := s.CreateSession(ctx, NewSession{UserID: u.ID, TokenHash: "tok2", ExpiresAt: "x"})
	if ss2.IP != nil || ss2.UserAgent != nil {
		t.Fatalf("empty ip/ua should be NULL: %+v", ss2)
	}

	// List (newest first), touch, revoke.
	if err := s.TouchSession(ctx, ss.ID); err != nil {
		t.Fatalf("TouchSession: %v", err)
	}
	list, err := s.ListSessions(ctx, u.ID)
	if err != nil || len(list) != 2 {
		t.Fatalf("ListSessions = %d, %v", len(list), err)
	}
	if list[0].ID != ss2.ID {
		t.Fatalf("ListSessions order: first = %d, want %d", list[0].ID, ss2.ID)
	}
	if err := s.RevokeSession(ctx, ss.ID); err != nil {
		t.Fatalf("RevokeSession: %v", err)
	}
	if err := s.RevokeSession(ctx, ss.ID); !errors.Is(err, sql.ErrNoRows) {
		t.Fatalf("RevokeSession twice = %v, want ErrNoRows", err)
	}
	if err := s.RevokeAllForUser(ctx, u.ID); err != nil {
		t.Fatalf("RevokeAllForUser: %v", err)
	}
	list, _ = s.ListSessions(ctx, u.ID)
	if len(list) != 0 {
		t.Fatalf("after RevokeAllForUser: %d sessions", len(list))
	}
}

func TestRevokeAllForUserExcept(t *testing.T) {
	ctx := context.Background()
	s := openStore(t)

	u, _ := s.CreateUser(ctx, NewUser{Username: "carol", PasswordHash: "h", Role: "user"})
	s1, err := s.CreateSession(ctx, NewSession{UserID: u.ID, TokenHash: "k1", ExpiresAt: "x"})
	if err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	s2, err := s.CreateSession(ctx, NewSession{UserID: u.ID, TokenHash: "k2", ExpiresAt: "x"})
	if err != nil {
		t.Fatalf("CreateSession: %v", err)
	}

	if err := s.RevokeAllForUserExcept(ctx, u.ID, s1.ID); err != nil {
		t.Fatalf("RevokeAllForUserExcept: %v", err)
	}
	if _, err := s.SessionByID(ctx, s1.ID); err != nil {
		t.Fatalf("kept session gone: %v", err)
	}
	if _, err := s.SessionByID(ctx, s2.ID); !errors.Is(err, sql.ErrNoRows) {
		t.Fatalf("other session survived: %v", err)
	}
}

func TestSessionsCascadeOnUserDelete(t *testing.T) {
	ctx := context.Background()
	s := openStore(t)

	u, _ := s.CreateUser(ctx, NewUser{Username: "bob", PasswordHash: "h", Role: "user"})
	if _, err := s.CreateSession(ctx, NewSession{UserID: u.ID, TokenHash: "tok", ExpiresAt: "x"}); err != nil {
		t.Fatalf("CreateSession: %v", err)
	}
	if err := s.DeleteUser(ctx, u.ID); err != nil {
		t.Fatalf("DeleteUser: %v", err)
	}
	if _, err := s.AuthSessionByTokenHash(ctx, "tok"); !errors.Is(err, sql.ErrNoRows) {
		t.Fatalf("session survived user delete: %v", err)
	}
}

func TestMeta(t *testing.T) {
	ctx := context.Background()
	s := openStore(t)

	if _, ok, err := s.GetMeta(ctx, "setup.done"); err != nil || ok {
		t.Fatalf("GetMeta missing = ok %v, err %v", ok, err)
	}
	if err := s.SetMeta(ctx, "setup.done", "1"); err != nil {
		t.Fatalf("SetMeta: %v", err)
	}
	if v, ok, err := s.GetMeta(ctx, "setup.done"); err != nil || !ok || v != "1" {
		t.Fatalf("GetMeta = %q %v %v", v, ok, err)
	}
	if err := s.SetMeta(ctx, "setup.done", "0"); err != nil {
		t.Fatalf("SetMeta overwrite: %v", err)
	}
	if v, _, _ := s.GetMeta(ctx, "setup.done"); v != "0" {
		t.Fatalf("GetMeta after overwrite = %q", v)
	}
}

func TestPurgeExpiredSessions(t *testing.T) {
	ctx := context.Background()
	s := openStore(t)
	u, _ := s.CreateUser(ctx, NewUser{Username: "alice", PasswordHash: "h", Role: "user"})

	now := time.Date(2026, 10, 28, 12, 0, 0, 0, time.UTC)
	old, _ := s.CreateSession(ctx, NewSession{UserID: u.ID, TokenHash: "old", ExpiresAt: "2026-10-28T11:59:59Z"})
	live, _ := s.CreateSession(ctx, NewSession{UserID: u.ID, TokenHash: "live", ExpiresAt: "2026-10-28T12:00:01Z"})

	n, err := s.PurgeExpiredSessions(ctx, now)
	if err != nil || n != 1 {
		t.Fatalf("PurgeExpiredSessions = %d, %v; want 1", n, err)
	}
	if _, err := s.SessionByID(ctx, old.ID); !errors.Is(err, sql.ErrNoRows) {
		t.Fatalf("expired session survived: %v", err)
	}
	if _, err := s.SessionByID(ctx, live.ID); err != nil {
		t.Fatalf("live session purged: %v", err)
	}
}
