package config

import (
	"fmt"
	"log/slog"
	"os"
	"strconv"
)

// Config holds all runtime configuration for hermesd.
// Every value is read from the environment at startup; there is no
// process-env access anywhere else in the codebase.
type Config struct {
	Host         string
	Port         int
	DatabasePath string
	LogLevel     string
}

// Load reads configuration from the environment, applying defaults.
func Load() (Config, error) {
	return Config{
		Host:         getenv("HOST", "0.0.0.0"),
		Port:         getenvInt("PORT", 3000),
		DatabasePath: getenv("DATABASE_PATH", "./data/hermes.sqlite"),
		LogLevel:     getenv("LOG_LEVEL", "info"),
	}, nil
}

// Addr returns the host:port the HTTP server should listen on.
func (c Config) Addr() string {
	return fmt.Sprintf("%s:%d", c.Host, c.Port)
}

// SlogLevel converts the configured level string to a slog.Level,
// falling back to info on unknown values.
func (c Config) SlogLevel() slog.Level {
	switch c.LogLevel {
	case "debug":
		return slog.LevelDebug
	case "warn":
		return slog.LevelWarn
	case "error":
		return slog.LevelError
	default:
		return slog.LevelInfo
	}
}

func getenv(key, fallback string) string {
	if v, ok := os.LookupEnv(key); ok && v != "" {
		return v
	}
	return fallback
}

func getenvInt(key string, fallback int) int {
	v, ok := os.LookupEnv(key)
	if !ok || v == "" {
		return fallback
	}
	n, err := strconv.Atoi(v)
	if err != nil {
		return fallback
	}
	return n
}
