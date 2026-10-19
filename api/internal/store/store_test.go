package store

import (
	"context"
	"path/filepath"
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

	// Connection pool is capped at a single writer (ADR-001 §R-1).
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
