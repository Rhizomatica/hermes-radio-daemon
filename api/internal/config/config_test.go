package config

import "testing"

func TestLoadDefault(t *testing.T) {
	t.Setenv("DATABASE_PATH", "")
	if got := Load().DatabasePath; got != defaultDatabase {
		t.Fatalf("Load().DatabasePath = %q, want %q", got, defaultDatabase)
	}
}

func TestLoadOverride(t *testing.T) {
	t.Setenv("DATABASE_PATH", "/tmp/hermes.sqlite")
	if got := Load().DatabasePath; got != "/tmp/hermes.sqlite" {
		t.Fatalf("Load().DatabasePath = %q, want %q", got, "/tmp/hermes.sqlite")
	}
}
