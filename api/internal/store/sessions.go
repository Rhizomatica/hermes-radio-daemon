package store

import (
	"context"
	"database/sql"
	"fmt"
	"time"
)

// Session is one row of the sessions table.
type Session struct {
	ID         int64
	UserID     int64
	TokenHash  string
	IP         *string
	UserAgent  *string
	CreatedAt  string
	LastSeenAt string
	ExpiresAt  string
}

// NewSession is what CreateSession needs; timestamps other than expires_at
// are defaulted by the table.
type NewSession struct {
	UserID    int64
	TokenHash string
	IP        string
	UserAgent string
	ExpiresAt string
}

// AuthSession is what an authentication check needs: a session joined to
// its user's role and disabled flag.
type AuthSession struct {
	SessionID  int64
	UserID     int64
	Role       string
	Disabled   bool
	LastSeenAt string
	ExpiresAt  string
}

const sessionCols = "id, user_id, token_hash, ip, user_agent, created_at, last_seen_at, expires_at"

// CreateSession inserts a session and returns the stored row.
func (s *Store) CreateSession(ctx context.Context, n NewSession) (Session, error) {
	res, err := s.db.ExecContext(ctx, `
		INSERT INTO sessions (user_id, token_hash, ip, user_agent, expires_at)
		VALUES (?, ?, ?, ?, ?)`,
		n.UserID, n.TokenHash, nullable(n.IP), nullable(n.UserAgent), n.ExpiresAt)
	if err != nil {
		return Session{}, fmt.Errorf("insert session: %w", err)
	}
	id, err := res.LastInsertId()
	if err != nil {
		return Session{}, err
	}
	return s.SessionByID(ctx, id)
}

// AuthSessionByTokenHash resolves a token hash to the joined session/user,
// or sql.ErrNoRows when there is no such session.
func (s *Store) AuthSessionByTokenHash(ctx context.Context, hash string) (AuthSession, error) {
	var a AuthSession
	var disabled int64
	err := s.db.QueryRowContext(ctx, `
		SELECT s.id, s.user_id, u.role, u.disabled, s.last_seen_at, s.expires_at
		FROM sessions s JOIN users u ON u.id = s.user_id
		WHERE s.token_hash = ?`, hash).Scan(&a.SessionID, &a.UserID, &a.Role, &disabled, &a.LastSeenAt, &a.ExpiresAt)
	if err != nil {
		return AuthSession{}, err
	}
	a.Disabled = disabled != 0
	return a, nil
}

// SessionByTokenHash returns one session, or sql.ErrNoRows.
func (s *Store) SessionByTokenHash(ctx context.Context, hash string) (Session, error) {
	return scanSession(s.db.QueryRowContext(ctx, `SELECT `+sessionCols+` FROM sessions WHERE token_hash = ?`, hash))
}

// SessionByID returns one session, or sql.ErrNoRows.
func (s *Store) SessionByID(ctx context.Context, id int64) (Session, error) {
	return scanSession(s.db.QueryRowContext(ctx, `SELECT `+sessionCols+` FROM sessions WHERE id = ?`, id))
}

// TouchSession bumps last_seen_at.
func (s *Store) TouchSession(ctx context.Context, id int64) error {
	_, err := s.db.ExecContext(ctx,
		`UPDATE sessions SET last_seen_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') WHERE id = ?`, id)
	return err
}

// RevokeSession deletes one session. It returns sql.ErrNoRows when none matched.
func (s *Store) RevokeSession(ctx context.Context, id int64) error {
	res, err := s.db.ExecContext(ctx, `DELETE FROM sessions WHERE id = ?`, id)
	if err != nil {
		return err
	}
	if n, err := res.RowsAffected(); err == nil && n == 0 {
		return sql.ErrNoRows
	}
	return nil
}

// RevokeAllForUser deletes every session of a user.
func (s *Store) RevokeAllForUser(ctx context.Context, userID int64) error {
	_, err := s.db.ExecContext(ctx, `DELETE FROM sessions WHERE user_id = ?`, userID)
	return err
}

// RevokeAllForUserExcept deletes every session of a user except one (the
// caller's own, on a self password change).
func (s *Store) RevokeAllForUserExcept(ctx context.Context, userID, exceptSessionID int64) error {
	_, err := s.db.ExecContext(ctx, `DELETE FROM sessions WHERE user_id = ? AND id != ?`, userID, exceptSessionID)
	return err
}

// PurgeExpiredSessions deletes every session that expired before now and
// returns how many. expires_at is UTC RFC 3339, so the string comparison
// orders by time.
func (s *Store) PurgeExpiredSessions(ctx context.Context, now time.Time) (int64, error) {
	res, err := s.db.ExecContext(ctx, `DELETE FROM sessions WHERE expires_at < ?`,
		now.UTC().Format(time.RFC3339))
	if err != nil {
		return 0, err
	}
	return res.RowsAffected()
}

// ListSessions returns a user's sessions, newest first.
func (s *Store) ListSessions(ctx context.Context, userID int64) ([]Session, error) {
	rows, err := s.db.QueryContext(ctx,
		`SELECT `+sessionCols+` FROM sessions WHERE user_id = ? ORDER BY id DESC`, userID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var out []Session
	for rows.Next() {
		ss, err := scanSession(rows)
		if err != nil {
			return nil, err
		}
		out = append(out, ss)
	}
	return out, rows.Err()
}

func scanSession(sc interface{ Scan(...any) error }) (Session, error) {
	var s Session
	var ip, ua sql.NullString
	if err := sc.Scan(&s.ID, &s.UserID, &s.TokenHash, &ip, &ua,
		&s.CreatedAt, &s.LastSeenAt, &s.ExpiresAt); err != nil {
		return Session{}, err
	}
	s.IP = ns(ip)
	s.UserAgent = ns(ua)
	return s, nil
}

// nullable turns "" into NULL, so an empty ip/user-agent is not stored.
func nullable(s string) *string {
	if s == "" {
		return nil
	}
	return &s
}
