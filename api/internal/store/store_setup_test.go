package store

import (
	"context"
	"testing"
)

func TestCompleteSetup(t *testing.T) {
	ctx := context.Background()
	s := openStore(t)

	u, err := s.CompleteSetup(ctx, NewUser{Username: "admin", PasswordHash: "h", Role: "admin"},
		map[string]string{"station.name": "Test Station", "setup.done": "1"})
	if err != nil {
		t.Fatalf("CompleteSetup: %v", err)
	}
	if u.Username != "admin" {
		t.Fatalf("user = %q", u.Username)
	}
	if v, ok, err := s.GetMeta(ctx, "setup.done"); err != nil || !ok || v != "1" {
		t.Fatalf("setup.done = %q %v %v", v, ok, err)
	}
	if v, _, _ := s.GetMeta(ctx, "station.name"); v != "Test Station" {
		t.Fatalf("station.name = %q", v)
	}
}

func TestCompleteSetupRollsBackOnError(t *testing.T) {
	ctx := context.Background()
	s := openStore(t)

	if _, err := s.CreateUser(ctx, NewUser{Username: "taken", PasswordHash: "h", Role: "admin"}); err != nil {
		t.Fatalf("CreateUser: %v", err)
	}
	if _, err := s.CompleteSetup(ctx, NewUser{Username: "taken", PasswordHash: "h", Role: "admin"},
		map[string]string{"station.name": "Test", "setup.done": "1"}); err == nil {
		t.Fatal("CompleteSetup with a taken username succeeded")
	}
	if _, ok, _ := s.GetMeta(ctx, "setup.done"); ok {
		t.Fatal("setup.done was written despite the failed setup")
	}
}
