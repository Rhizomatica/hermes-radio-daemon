package store

import (
	"context"
	"fmt"
)

// CompleteSetup atomically records first-boot setup: it creates the first
// admin user and writes the station identity and the setup.done flag in one
// transaction, so a partial failure cannot leave an admin without setup.done
// (or the flag without an admin).
func (s *Store) CompleteSetup(ctx context.Context, u NewUser, metas map[string]string) (User, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return User{}, err
	}
	defer tx.Rollback() // no-op after Commit

	res, err := tx.ExecContext(ctx, `
		INSERT INTO users (username, password_hash, role, display_name, locale, phone, location)
		VALUES (?, ?, ?, ?, ?, ?, ?)`,
		u.Username, u.PasswordHash, u.Role, u.DisplayName, u.Locale, u.Phone, u.Location)
	if err != nil {
		return User{}, fmt.Errorf("insert user %q: %w", u.Username, err)
	}
	id, err := res.LastInsertId()
	if err != nil {
		return User{}, err
	}
	for k, v := range metas {
		if _, err := tx.ExecContext(ctx,
			`INSERT INTO meta (key, value) VALUES (?, ?)
			 ON CONFLICT(key) DO UPDATE SET value = excluded.value`, k, v); err != nil {
			return User{}, err
		}
	}
	if err := tx.Commit(); err != nil {
		return User{}, err
	}
	return s.UserByID(ctx, id)
}
