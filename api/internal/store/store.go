// Package store provides the SQLite persistence layer for hermesd.
//
// It owns the single write connection (ADR-001 §R-1) and the embedded
// migration runner. Every connection is configured for WAL journaling,
// foreign-key enforcement, a busy timeout, and synchronous=NORMAL durability.
package store

import (
	"context"
	"database/sql"
	"fmt"
	"os"
	"path/filepath"

	_ "modernc.org/sqlite" // registers the "sqlite" database/sql driver
)

// Store wraps the hermesd SQLite database. Use Open to construct one.
type Store struct {
	db *sql.DB
}

// Open connects to the SQLite database at path, applies the connection pragmas,
// and runs any pending migrations. It creates the parent directory if needed.
//
// The connection pool is capped at a single connection to re-establish the
// single-writer serialization the Node.js event loop previously provided for
// free (ADR-001 §R-1).
func Open(ctx context.Context, path string) (*Store, error) {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return nil, fmt.Errorf("create database directory: %w", err)
	}

	db, err := sql.Open("sqlite", dsn(path))
	if err != nil {
		return nil, fmt.Errorf("open sqlite: %w", err)
	}

	// Single writer: one connection in the pool, kept warm.
	db.SetMaxOpenConns(1)
	db.SetMaxIdleConns(1)
	db.SetConnMaxLifetime(0)

	if err := db.PingContext(ctx); err != nil {
		_ = db.Close()
		return nil, fmt.Errorf("ping sqlite: %w", err)
	}

	s := &Store{db: db}
	if err := s.migrate(ctx); err != nil {
		_ = db.Close()
		return nil, fmt.Errorf("migrate: %w", err)
	}

	return s, nil
}

// Close releases the underlying database connection.
func (s *Store) Close() error {
	return s.db.Close()
}

// PingContext reports whether the database is reachable. It satisfies the
// server.Pinger interface used by /health.
func (s *Store) PingContext(ctx context.Context) error {
	return s.db.PingContext(ctx)
}

// dsn builds the modernc.org/sqlite DSN with the per-connection pragmas
// hermesd relies on. Each _pragma value is executed as "PRAGMA <value>" when a
// connection opens, so the settings survive connection re-establishment. The
// driver applies busy_timeout first regardless of order here.
func dsn(path string) string {
	return path +
		"?_pragma=busy_timeout(5000)" +
		"&_pragma=foreign_keys(1)" +
		"&_pragma=synchronous(NORMAL)" +
		"&_pragma=journal_mode(WAL)"
}
