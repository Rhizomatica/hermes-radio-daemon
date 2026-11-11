package store

import (
	"context"
	"database/sql"
	"errors"
	"path/filepath"
	"testing"
)

func openTrips(t *testing.T) (*Store, context.Context) {
	t.Helper()
	s, err := Open(context.Background(), filepath.Join(t.TempDir(), "hermes.sqlite"))
	if err != nil {
		t.Fatalf("Open: %v", err)
	}
	t.Cleanup(func() { s.Close() })
	return s, context.Background()
}

func TestTripLifecycle(t *testing.T) {
	s, ctx := openTrips(t)

	if _, ok, err := s.CurrentTrip(ctx); err != nil || ok {
		t.Fatalf("CurrentTrip on empty = ok %v, err %v; want none", ok, err)
	}

	tr, err := s.OpenTrip(ctx, "Field day")
	if err != nil {
		t.Fatalf("OpenTrip: %v", err)
	}
	if tr.ID == 0 || tr.Name != "Field day" || tr.StartedAt == "" || tr.EndedAt != nil {
		t.Fatalf("open trip = %+v", tr)
	}
	if _, ok, err := s.CurrentTrip(ctx); err != nil || !ok {
		t.Fatalf("CurrentTrip after open = ok %v, err %v", ok, err)
	}

	// The schema, not just the caller, keeps a second trip from opening.
	if _, err := s.OpenTrip(ctx, "Another"); !errors.Is(err, ErrTripOpen) {
		t.Fatalf("second OpenTrip = %v, want ErrTripOpen", err)
	}

	closed, err := s.CloseTrip(ctx, tr.ID)
	if err != nil {
		t.Fatalf("CloseTrip: %v", err)
	}
	if closed.EndedAt == nil {
		t.Fatalf("closed trip ended_at = nil")
	}
	if _, ok, _ := s.CurrentTrip(ctx); ok {
		t.Fatal("CurrentTrip after CloseTrip: still open")
	}
	// Closing a trip that is not open: no row changes.
	if _, err := s.CloseTrip(ctx, tr.ID); !errors.Is(err, sql.ErrNoRows) {
		t.Fatalf("CloseTrip closed again = %v, want sql.ErrNoRows", err)
	}

	// A closed trip no longer blocks a new one.
	if _, err := s.OpenTrip(ctx, "Second"); err != nil {
		t.Fatalf("OpenTrip after close: %v", err)
	}
}

func TestListTripsNewestFirst(t *testing.T) {
	s, ctx := openTrips(t)
	a, _ := s.OpenTrip(ctx, "a")
	if _, err := s.CloseTrip(ctx, a.ID); err != nil {
		t.Fatal(err)
	}
	b, _ := s.OpenTrip(ctx, "b")
	if _, err := s.CloseTrip(ctx, b.ID); err != nil {
		t.Fatal(err)
	}

	trips, next, err := s.ListTrips(ctx, 50, 0)
	if err != nil {
		t.Fatal(err)
	}
	if len(trips) != 2 || trips[0].ID != b.ID || trips[0].Name != "b" ||
		trips[1].ID != a.ID || trips[1].Name != "a" || next != 0 {
		t.Fatalf("ListTrips = %+v next %d", trips, next)
	}

	// One per page, newest first, cursor is the last id returned.
	page, next, err := s.ListTrips(ctx, 1, 0)
	if err != nil || len(page) != 1 || page[0].ID != b.ID || next != b.ID {
		t.Fatalf("ListTrips page 1 = %+v next %d err %v", page, next, err)
	}
	page, next, err = s.ListTrips(ctx, 1, next)
	if err != nil || len(page) != 1 || page[0].ID != a.ID || next != 0 {
		t.Fatalf("ListTrips page 2 = %+v next %d err %v", page, next, err)
	}
}

func TestTripByIDMissing(t *testing.T) {
	s, ctx := openTrips(t)
	if _, err := s.TripByID(ctx, 999999); !errors.Is(err, sql.ErrNoRows) {
		t.Fatalf("TripByID missing = %v, want sql.ErrNoRows", err)
	}
}
