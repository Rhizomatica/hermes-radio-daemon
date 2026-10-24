// Package config reads the API layer's slice of radiod's configuration. The
// API layer owns no socket: radiod's C server listens and hands /api/v2
// requests to the Go exports, so the only value the Go layer reads is where
// its SQLite database lives.
package config

import "os"

// defaultDatabase is the API's SQLite database, under radiod's data directory
// (common/cfg_utils.c main:recording_dir, /var/lib/hermes-radio-daemon).
const defaultDatabase = "/var/lib/hermes-radio-daemon/hermes.sqlite"

// defaultSetupCode is where the installer writes the one-time setup code
// (api-rest.yaml: /etc/hermes/setup-code, root only).
const defaultSetupCode = "/etc/hermes/setup-code"

// Config is the slice of radiod's configuration the API layer reads.
type Config struct {
	// DatabasePath is where the API's SQLite database lives. DATABASE_PATH
	// overrides it (the self-check uses this); the default is absolute, not
	// relative to the process working directory.
	DatabasePath string
	// SetupCodePath is where the installer's one-time first-boot code lives
	// (root only). SETUP_CODE_PATH overrides it.
	SetupCodePath string
}

// Load returns the paths the API layer reads: the env overrides when set,
// otherwise the absolute defaults.
func Load() Config {
	return Config{
		DatabasePath:  getenv("DATABASE_PATH", defaultDatabase),
		SetupCodePath: getenv("SETUP_CODE_PATH", defaultSetupCode),
	}
}

func getenv(key, fallback string) string {
	if v, ok := os.LookupEnv(key); ok && v != "" {
		return v
	}
	return fallback
}
