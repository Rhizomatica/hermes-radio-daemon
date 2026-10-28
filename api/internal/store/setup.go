package store

import (
	"context"
	"errors"
)

// ErrSetupDone is returned by CompleteSetup when first-boot setup was
// already completed.
var ErrSetupDone = errors.New("setup already done")

// CompleteSetup atomically records first-boot setup: it claims setup.done,
// creates the first admin user and writes the station identity in one
// transaction, so a partial failure cannot leave an admin without setup.done
// (or the flag without an admin). The claim is the first write, so two
// concurrent setups cannot both succeed: the second gets ErrSetupDone.
func (s *Store) CompleteSetup(ctx context.Context, u NewUser, metas map[string]string) (User, error) {
	tx, err := s.db.BeginTx(ctx, nil)
	if err != nil {
		return User{}, err
	}
	defer tx.Rollback() // no-op after Commit

	// Inserts the flag, or flips it from anything but "1"; touches no row
	// when setup is already done.
	claim, err := tx.ExecContext(ctx, `
		INSERT INTO meta (key, value) VALUES ('setup.done', '1')
		ON CONFLICT(key) DO UPDATE SET value = excluded.value WHERE meta.value != '1'`)
	if err != nil {
		return User{}, err
	}
	if n, err := claim.RowsAffected(); err != nil {
		return User{}, err
	} else if n == 0 {
		return User{}, ErrSetupDone
	}

	res, err := tx.ExecContext(ctx, `
		INSERT INTO users (username, password_hash, role, display_name, locale, phone, location)
		VALUES (?, ?, ?, ?, ?, ?, ?)`,
		u.Username, u.PasswordHash, u.Role, u.DisplayName, u.Locale, u.Phone, u.Location)
	if err != nil {
		return User{}, insertUserErr(u.Username, err)
	}
	id, err := res.LastInsertId()
	if err != nil {
		return User{}, err
	}
	for k, v := range metas {
		if k == "setup.done" {
			continue // claimed above
		}
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
