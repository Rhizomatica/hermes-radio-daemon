-- 0004_create_trips.sql
--
-- The station's recorded trips: named time ranges of its track
-- (api/internal/gps/track.go). The trip's positions are the track's points at
-- times started_at..ended_at, so there is no points table.
--
-- A trip is opened explicitly (POST /gps/trip/open) and closed explicitly
-- (POST /gps/trip/close); closing does not open a new one. At most one trip is
-- open at a time, enforced in the schema so that two processes sharing the
-- database cannot each open one.

CREATE TABLE trips (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    name       TEXT    NOT NULL,
    started_at TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    ended_at   TEXT
);

-- One open trip (ended_at IS NULL) in the database, whichever process opened it.
CREATE UNIQUE INDEX trips_one_open ON trips ((1)) WHERE ended_at IS NULL;

-- Listing trips newest first.
CREATE INDEX trips_started_at ON trips (started_at DESC);
