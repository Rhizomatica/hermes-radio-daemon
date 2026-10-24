-- 0002_create_sessions.sql
--
-- The sessions table: opaque login tokens, stored only as hashes. A session
-- row is what the router's RoleOf resolves a request against; its expires_at
-- bounds how long a session lives, and revoking is a row delete.

CREATE TABLE sessions (
    id           INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id      INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    token_hash   TEXT    NOT NULL UNIQUE,
    ip           TEXT,
    user_agent   TEXT,
    created_at   TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    last_seen_at TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    expires_at   TEXT    NOT NULL
);

CREATE INDEX idx_sessions_user_id ON sessions(user_id);
