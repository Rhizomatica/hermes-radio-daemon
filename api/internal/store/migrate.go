package store

import (
	"context"
	"embed"
	"fmt"
	"io/fs"
	"sort"
)

// migrationsFS holds the ordered SQL migration files. They are applied in
// lexicographic order by filename, so name them "NNNN_description.sql".
//
//go:embed migrations/*.sql
var migrationsFS embed.FS

// migrate applies any migrations not yet recorded in schema_migrations. Each
// migration runs in its own transaction together with its bookkeeping row, so
// a failed migration leaves the database unchanged.
func (s *Store) migrate(ctx context.Context) error {
	if _, err := s.db.ExecContext(ctx, `CREATE TABLE IF NOT EXISTS schema_migrations (
		version    TEXT PRIMARY KEY,
		applied_at TEXT NOT NULL
	)`); err != nil {
		return fmt.Errorf("create schema_migrations: %w", err)
	}

	entries, err := fs.Glob(migrationsFS, "migrations/*.sql")
	if err != nil {
		return fmt.Errorf("list migrations: %w", err)
	}
	sort.Strings(entries)

	for _, name := range entries {
		applied, err := s.isApplied(ctx, name)
		if err != nil {
			return err
		}
		if applied {
			continue
		}

		body, err := migrationsFS.ReadFile(name)
		if err != nil {
			return fmt.Errorf("read %s: %w", name, err)
		}
		if err := s.apply(ctx, name, string(body)); err != nil {
			return fmt.Errorf("apply %s: %w", name, err)
		}
	}

	return nil
}

func (s *Store) isApplied(ctx context.Context, name string) (bool, error) {
	var applied bool
	err := s.db.QueryRowContext(ctx,
		`SELECT EXISTS(SELECT 1 FROM schema_migrations WHERE version = ?)`, name,
	).Scan(&applied)
	if err != nil {
		return false, fmt.Errorf("check %s: %w", name, err)
	}
	return applied, nil
}

func (s *Store) apply(ctx context.Context, name, body string) error {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback() // no-op after a successful Commit

	if _, err := tx.ExecContext(ctx, body); err != nil {
		return err
	}
	if _, err := tx.ExecContext(ctx,
		`INSERT INTO schema_migrations (version, applied_at) VALUES (?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))`,
		name,
	); err != nil {
		return err
	}
	return tx.Commit()
}
