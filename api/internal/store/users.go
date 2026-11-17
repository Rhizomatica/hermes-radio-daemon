package store

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"strings"
	"unicode/utf8"

	"modernc.org/sqlite"
	sqlite3 "modernc.org/sqlite/lib"
)

// ErrUsernameTaken is returned when a user insert violates the unique
// username constraint.
var ErrUsernameTaken = errors.New("username is taken")

// ErrLastAdmin is returned when a change would leave no enabled admin.
var ErrLastAdmin = errors.New("cannot remove the last active admin")

// ErrInvalidLimit is returned when a page size is not positive.
var ErrInvalidLimit = errors.New("invalid limit")

// MaxDisplayName is api-rest.yaml's display_name maxLength.
const MaxDisplayName = 64

// MinPasswordLength is api-rest.yaml's password minLength.
const MinPasswordLength = 8

// ValidDisplayName reports whether s fits the display_name maxLength,
// counted in characters rather than bytes.
func ValidDisplayName(s string) bool { return utf8.RuneCountInString(s) <= MaxDisplayName }

// isUniqueViolation reports whether err is SQLite's UNIQUE constraint error.
func isUniqueViolation(err error) bool {
	var se *sqlite.Error
	return errors.As(err, &se) && se.Code() == sqlite3.SQLITE_CONSTRAINT_UNIQUE
}

// insertUserErr wraps a failed users insert: ErrUsernameTaken for a duplicate.
func insertUserErr(username string, err error) error {
	if isUniqueViolation(err) {
		return fmt.Errorf("insert user %q: %w", username, ErrUsernameTaken)
	}
	return fmt.Errorf("insert user %q: %w", username, err)
}

// User is one row of the users table.
type User struct {
	ID           int64
	Username     string
	PasswordHash string
	Role         string
	DisplayName  *string
	Locale       *string
	Phone        *string
	Location     *string
	Disabled     bool
	LastLoginAt  *string
	CreatedAt    string
	UpdatedAt    string
}

// NewUser is what CreateUser needs; it does not include timestamps, which
// the table defaults.
type NewUser struct {
	Username     string
	PasswordHash string
	Role         string
	DisplayName  *string
	Locale       *string
	Phone        *string
	Location     *string
}

// UserPatch is a partial update. A nil pointer leaves the column unchanged;
// ClearPhone and ClearLocation set their columns to NULL.
type UserPatch struct {
	DisplayName   *string
	Role          *string
	Locale        *string
	Phone         *string
	Location      *string
	Disabled      *bool
	ClearPhone    bool
	ClearLocation bool
}

const userCols = "id, username, password_hash, role, display_name, locale, phone, location, disabled, last_login_at, created_at, updated_at"

// CreateUser inserts a user and returns the stored row.
func (s *Store) CreateUser(ctx context.Context, n NewUser) (User, error) {
	res, err := s.db.ExecContext(ctx, `
		INSERT INTO users (username, password_hash, role, display_name, locale, phone, location)
		VALUES (?, ?, ?, ?, ?, ?, ?)`,
		n.Username, n.PasswordHash, n.Role, n.DisplayName, n.Locale, n.Phone, n.Location)
	if err != nil {
		return User{}, insertUserErr(n.Username, err)
	}
	id, err := res.LastInsertId()
	if err != nil {
		return User{}, err
	}
	return s.UserByID(ctx, id)
}

// UserByID returns one user, or sql.ErrNoRows.
func (s *Store) UserByID(ctx context.Context, id int64) (User, error) {
	return scanUser(s.db.QueryRowContext(ctx, `SELECT `+userCols+` FROM users WHERE id = ?`, id))
}

// UserByUsername returns one user, or sql.ErrNoRows.
func (s *Store) UserByUsername(ctx context.Context, username string) (User, error) {
	return scanUser(s.db.QueryRowContext(ctx, `SELECT `+userCols+` FROM users WHERE username = ?`, username))
}

// ListUsers returns one page of users ordered by id, and the id to use as
// the next cursor (0 on the last page).
func (s *Store) ListUsers(ctx context.Context, limit int, cursor int64) ([]User, int64, error) {
	if limit < 1 {
		return nil, 0, fmt.Errorf("%w: %d", ErrInvalidLimit, limit)
	}
	// One extra row tells whether another page follows, so a full last page
	// does not hand out a cursor to an empty one.
	rows, err := s.db.QueryContext(ctx, `SELECT `+userCols+` FROM users WHERE id > ? ORDER BY id ASC LIMIT ?`, cursor, limit+1)
	if err != nil {
		return nil, 0, err
	}
	defer rows.Close()
	var out []User
	for rows.Next() {
		u, err := scanUser(rows)
		if err != nil {
			return nil, 0, err
		}
		out = append(out, u)
	}
	if err := rows.Err(); err != nil {
		return nil, 0, err
	}
	var next int64
	if len(out) > limit {
		out = out[:limit]
		next = out[len(out)-1].ID
	}
	return out, next, nil
}

// UpdateUser applies a partial update and returns the stored row.
func (s *Store) UpdateUser(ctx context.Context, id int64, p UserPatch) (User, error) {
	return s.updateUser(ctx, id, p, false)
}

// UpdateUserKeepingActiveAdmin is UpdateUser, but refuses with ErrLastAdmin
// when the update would leave no enabled admin. The count and the write are
// one statement, so concurrent requests cannot both remove the last admin.
func (s *Store) UpdateUserKeepingActiveAdmin(ctx context.Context, id int64, p UserPatch) (User, error) {
	return s.updateUser(ctx, id, p, true)
}

func (s *Store) updateUser(ctx context.Context, id int64, p UserPatch, keepActiveAdmin bool) (User, error) {
	sets := []string{"updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')"}
	var args []any
	add := func(col string, v any) {
		sets = append(sets, col+" = ?")
		args = append(args, v)
	}
	if p.DisplayName != nil {
		add("display_name", *p.DisplayName)
	}
	if p.Role != nil {
		add("role", *p.Role)
	}
	if p.Locale != nil {
		add("locale", *p.Locale)
	}
	if p.Phone != nil {
		add("phone", *p.Phone)
	}
	if p.ClearPhone {
		add("phone", nil)
	}
	if p.Location != nil {
		add("location", *p.Location)
	}
	if p.ClearLocation {
		add("location", nil)
	}
	if p.Disabled != nil {
		if *p.Disabled {
			add("disabled", 1)
		} else {
			add("disabled", 0)
		}
	}
	args = append(args, id)
	query := `UPDATE users SET ` + strings.Join(sets, ", ") + ` WHERE id = ?`
	if keepActiveAdmin {
		query += ` AND (SELECT count(*) FROM users WHERE role = 'admin' AND disabled = 0 AND id != ?) > 0`
		args = append(args, id)
	}
	res, err := s.db.ExecContext(ctx, query, args...)
	if err != nil {
		return User{}, err
	}
	if n, err := res.RowsAffected(); err == nil && n == 0 {
		return User{}, s.noRowErr(ctx, id, keepActiveAdmin)
	}
	return s.UserByID(ctx, id)
}

// DeleteUser removes a user. It returns sql.ErrNoRows when none matched.
func (s *Store) DeleteUser(ctx context.Context, id int64) error {
	return s.deleteUser(ctx, id, false)
}

// DeleteUserKeepingActiveAdmin is DeleteUser, but refuses with ErrLastAdmin
// when deleting the user would leave no enabled admin.
func (s *Store) DeleteUserKeepingActiveAdmin(ctx context.Context, id int64) error {
	return s.deleteUser(ctx, id, true)
}

func (s *Store) deleteUser(ctx context.Context, id int64, keepActiveAdmin bool) error {
	query := `DELETE FROM users WHERE id = ?`
	args := []any{id}
	if keepActiveAdmin {
		query += ` AND (SELECT count(*) FROM users WHERE role = 'admin' AND disabled = 0 AND id != ?) > 0`
		args = append(args, id)
	}
	res, err := s.db.ExecContext(ctx, query, args...)
	if err != nil {
		return err
	}
	if n, err := res.RowsAffected(); err == nil && n == 0 {
		return s.noRowErr(ctx, id, keepActiveAdmin)
	}
	return nil
}

// noRowErr tells a missing row from an active-admin guard refusal.
func (s *Store) noRowErr(ctx context.Context, id int64, guarded bool) error {
	if guarded {
		var one int
		err := s.db.QueryRowContext(ctx, `SELECT 1 FROM users WHERE id = ?`, id).Scan(&one)
		if err == nil {
			return ErrLastAdmin
		}
		if !errors.Is(err, sql.ErrNoRows) {
			return err
		}
	}
	return sql.ErrNoRows
}

// SetPasswordHash replaces a user's password hash and bumps updated_at.
func (s *Store) SetPasswordHash(ctx context.Context, id int64, hash string) error {
	res, err := s.db.ExecContext(ctx, `
		UPDATE users SET password_hash = ?, updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') WHERE id = ?`,
		hash, id)
	if err != nil {
		return err
	}
	if n, err := res.RowsAffected(); err == nil && n == 0 {
		return sql.ErrNoRows
	}
	return nil
}

// SetLastLoginAt records a successful login's timestamp.
func (s *Store) SetLastLoginAt(ctx context.Context, id int64) error {
	_, err := s.db.ExecContext(ctx,
		`UPDATE users SET last_login_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') WHERE id = ?`, id)
	return err
}

// CountUsers returns the number of users.
func (s *Store) CountUsers(ctx context.Context) (int, error) {
	var n int
	err := s.db.QueryRowContext(ctx, `SELECT count(*) FROM users`).Scan(&n)
	return n, err
}

func scanUser(sc interface{ Scan(...any) error }) (User, error) {
	var u User
	var display, locale, phone, location, lastLogin sql.NullString
	var disabled int64
	if err := sc.Scan(&u.ID, &u.Username, &u.PasswordHash, &u.Role,
		&display, &locale, &phone, &location, &disabled, &lastLogin,
		&u.CreatedAt, &u.UpdatedAt); err != nil {
		return User{}, err
	}
	u.DisplayName = ns(display)
	u.Locale = ns(locale)
	u.Phone = ns(phone)
	u.Location = ns(location)
	u.LastLoginAt = ns(lastLogin)
	u.Disabled = disabled != 0
	return u, nil
}

func ns(s sql.NullString) *string {
	if s.Valid {
		return &s.String
	}
	return nil
}
