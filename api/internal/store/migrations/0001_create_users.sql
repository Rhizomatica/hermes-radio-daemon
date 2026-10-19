-- 0001_create_users.sql
--
-- Seed table for ADR-002 (opaque session-token auth + four-role RBAC).
--
-- The rest of the schema is intentionally absent:
--   * sessions / rate-limit state  -> Phase 2 (auth core)
--   * messages, schedules, stations -> Phase 3 (REST), blocked on O-01/O-02
--
-- Each migration file is applied atomically. Never edit an already-released
-- migration — add a new file instead.

CREATE TABLE users (
    id            INTEGER PRIMARY KEY AUTOINCREMENT,
    username      TEXT    NOT NULL UNIQUE,
    password_hash TEXT    NOT NULL,
    role          TEXT    NOT NULL DEFAULT 'user'
        CHECK (role IN ('readonly', 'user', 'operator', 'admin')),
    created_at    TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    updated_at    TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);
