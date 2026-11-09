-- 0004_create_trips.sql
--
-- The station's recorded trips (breadcrumbs) and their position points,
-- matching api-rest.yaml's Trip and GpsPoint schemas.
--
-- A trip is opened explicitly (POST /gps/trip/open) and closed explicitly
-- (POST /gps/trip/close); closing does not open a new one, so at most one
-- trip is open at a time (ended_at IS NULL).

CREATE TABLE trips (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    started_at TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
    ended_at   TEXT
);

CREATE TABLE trip_points (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    trip_id    INTEGER NOT NULL REFERENCES trips(id) ON DELETE CASCADE,
    at         TEXT    NOT NULL,
    latitude   REAL    NOT NULL,
    longitude  REAL    NOT NULL,
    altitude_m REAL,
    accuracy_m REAL
);

CREATE INDEX trip_points_trip_id ON trip_points (trip_id, id);
