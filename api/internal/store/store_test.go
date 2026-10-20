package store

import (
	"context"
	"path/filepath"
	"strings"
	"testing"
)

func TestOpenMigratesAndConfiguresPragmas(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "hermes.sqlite")

	s, err := Open(ctx, path)
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer s.Close()

	// Connection pool is capped at a single writer, so writes are serialised.
	if got := s.db.Stats().MaxOpenConnections; got != 1 {
		t.Fatalf("MaxOpenConnections: want 1, got %d", got)
	}

	// Ping reports liveness.
	if err := s.PingContext(ctx); err != nil {
		t.Fatalf("PingContext: %v", err)
	}

	// WAL mode is active.
	var journalMode string
	if err := s.db.QueryRowContext(ctx, "PRAGMA journal_mode").Scan(&journalMode); err != nil {
		t.Fatalf("journal_mode: %v", err)
	}
	if journalMode != "wal" {
		t.Fatalf("journal_mode: want wal, got %s", journalMode)
	}

	// Foreign keys are enforced.
	var foreignKeys int
	if err := s.db.QueryRowContext(ctx, "PRAGMA foreign_keys").Scan(&foreignKeys); err != nil {
		t.Fatalf("foreign_keys: %v", err)
	}
	if foreignKeys != 1 {
		t.Fatalf("foreign_keys: want 1, got %d", foreignKeys)
	}

	// The users table was created by migration 0001.
	if _, err := s.db.ExecContext(ctx,
		`INSERT INTO users (username, password_hash, role) VALUES ('alice', 'x', 'admin')`); err != nil {
		t.Fatalf("insert into users: %v", err)
	}

	// The role CHECK constraint rejects unknown roles.
	if _, err := s.db.ExecContext(ctx,
		`INSERT INTO users (username, password_hash, role) VALUES ('bob', 'x', 'root')`); err == nil {
		t.Fatal("insert with unknown role: expected error, got nil")
	}
}

func TestOpenIsIdempotent(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "hermes.sqlite")

	s, err := Open(ctx, path)
	if err != nil {
		t.Fatalf("first Open: %v", err)
	}
	if err := s.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}

	// Reopening the same file must not re-apply migrations or error.
	s, err = Open(ctx, path)
	if err != nil {
		t.Fatalf("second Open: %v", err)
	}
	defer s.Close()

	var count int
	if err := s.db.QueryRowContext(ctx, `SELECT count(*) FROM schema_migrations`).Scan(&count); err != nil {
		t.Fatalf("count schema_migrations: %v", err)
	}
	if count != 1 {
		t.Fatalf("schema_migrations count: want 1, got %d", count)
	}
}

func TestUsersSchema(t *testing.T) {
	ctx := context.Background()
	s, err := Open(ctx, filepath.Join(t.TempDir(), "hermes.sqlite"))
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	defer s.Close()

	// Every api-rest.yaml User column inserts; disabled defaults to 0 (false).
	if _, err := s.db.ExecContext(ctx, `
		INSERT INTO users (username, password_hash, role, display_name, locale, phone, location, last_login_at)
		VALUES ('alice', 'x', 'admin', 'Alice', 'en', '+1 555 0100', 'Station A', '2026-10-20T00:00:00Z')`); err != nil {
		t.Fatalf("insert full user: %v", err)
	}
	var disabled int
	if err := s.db.QueryRowContext(ctx,
		`SELECT disabled FROM users WHERE username = 'alice'`).Scan(&disabled); err != nil {
		t.Fatalf("select disabled: %v", err)
	}
	if disabled != 0 {
		t.Fatalf("disabled default: want 0, got %d", disabled)
	}

	// username must match api-rest.yaml's pattern ^[a-z0-9._-]{1,32}$.
	for _, name := range []string{"Alice", "a/b", strings.Repeat("a", 33)} {
		if _, err := s.db.ExecContext(ctx,
			`INSERT INTO users (username, password_hash, role) VALUES (?, 'x', 'user')`, name); err == nil {
			t.Fatalf("username %q: expected CHECK error, got nil", name)
		}
	}
}
