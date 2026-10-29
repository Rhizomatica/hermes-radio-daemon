-- 0003_create_meta.sql
--
-- A small key/value table for station-wide markers the API owns but that do
-- not fit a domain table: first-boot setup state and the station identity
-- the setup wizard records.

CREATE TABLE meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
