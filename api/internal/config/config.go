// Package config reads the API layer's slice of radiod's configuration. The
// API layer owns no socket: radiod's C server listens and hands /api/v2
// requests to the Go exports, so the only value the Go layer reads is where
// its SQLite database lives.
package config

import "os"

// defaultDatabase is the API's SQLite database, under radiod's data directory
// (common/cfg_utils.c main:recording_dir, /var/lib/hermes-radio-daemon).
const defaultDatabase = "/var/lib/hermes-radio-daemon/hermes.sqlite"

// Config is the slice of radiod's configuration the API layer reads.
type Config struct {
	// DatabasePath is where the API's SQLite database lives. DATABASE_PATH
	// overrides it (the self-check uses this); the default is absolute, not
	// relative to the process working directory.
	DatabasePath string
}

// Load returns the database path: the DATABASE_PATH override when set,
// otherwise the absolute default under radiod's data directory.
func Load() Config {
	return Config{DatabasePath: getenv("DATABASE_PATH", defaultDatabase)}
}

func getenv(key, fallback string) string {
	if v, ok := os.LookupEnv(key); ok && v != "" {
		return v
	}
	return fallback
}
