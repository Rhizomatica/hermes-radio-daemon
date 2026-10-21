-- 0001_create_users.sql
--
-- The users table, matching api-rest.yaml's User schema.
--
-- Each migration file is applied atomically. Never edit an already-released
-- migration — add a new file instead.

CREATE TABLE users (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    username      TEXT    NOT NULL UNIQUE
        CHECK (length(username) BETWEEN 1 AND 32 AND username NOT GLOB '*[^a-z0-9._-]*'),
    password_hash TEXT    NOT NULL,
    role          TEXT    NOT NULL DEFAULT 'user'
        CHECK (role IN ('readonly', 'user', 'operator', 'admin')),
    display_name  TEXT,
    locale        TEXT,
    phone         TEXT,
    location      TEXT,
    disabled      INTEGER NOT NULL DEFAULT 0,
    last_login_at TEXT,
    created_at    TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at    TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);
