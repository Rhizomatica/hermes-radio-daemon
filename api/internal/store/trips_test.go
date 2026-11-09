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

	tr, err := s.OpenTrip(ctx)
	if err != nil {
		t.Fatalf("OpenTrip: %v", err)
	}
	if tr.ID == 0 || tr.StartedAt == "" || tr.EndedAt != nil || tr.PointCount != 0 {
		t.Fatalf("open trip = %+v", tr)
	}
	if _, ok, err := s.CurrentTrip(ctx); err != nil || !ok {
		t.Fatalf("CurrentTrip after open = ok %v, err %v", ok, err)
	}

	alt := 2240.0
	for _, p := range []NewTripPoint{
		{At: "2026-11-09T12:00:00Z", Latitude: 4.5, Longitude: -75.7},
		{At: "2026-11-09T12:00:01Z", Latitude: 4.6, Longitude: -75.8, AltitudeM: &alt},
	} {
		if err := s.AddTripPoint(ctx, tr.ID, p); err != nil {
			t.Fatalf("AddTripPoint: %v", err)
		}
	}

	cur, ok, err := s.CurrentTrip(ctx)
	if err != nil || !ok || cur.PointCount != 2 {
		t.Fatalf("CurrentTrip point_count = %v, ok %v, err %v", cur.PointCount, ok, err)
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
}

func TestTripPointsOrderAndPagination(t *testing.T) {
	s, ctx := openTrips(t)
	tr, _ := s.OpenTrip(ctx)
	for i := 1; i <= 5; i++ {
		if err := s.AddTripPoint(ctx, tr.ID, NewTripPoint{
			At: "2026-11-09T12:00:00Z", Latitude: float64(i), Longitude: 0}); err != nil {
			t.Fatalf("AddTripPoint %d: %v", i, err)
		}
	}

	// Oldest first.
	pts, next, err := s.TripPoints(ctx, tr.ID, 2, 0)
	if err != nil {
		t.Fatal(err)
	}
	if len(pts) != 2 || pts[0].Latitude != 1 || pts[1].Latitude != 2 || next != pts[1].ID {
		t.Fatalf("page 1 = %+v next %d", pts, next)
	}
	pts, next, err = s.TripPoints(ctx, tr.ID, 2, next)
	if err != nil {
		t.Fatal(err)
	}
	if len(pts) != 2 || pts[0].Latitude != 3 || pts[1].Latitude != 4 || next != pts[1].ID {
		t.Fatalf("page 2 = %+v next %d", pts, next)
	}
	pts, next, err = s.TripPoints(ctx, tr.ID, 2, next)
	if err != nil {
		t.Fatal(err)
	}
	if len(pts) != 1 || pts[0].Latitude != 5 || next != 0 {
		t.Fatalf("page 3 = %+v next %d", pts, next)
	}
}

func TestListTripsNewestFirst(t *testing.T) {
	s, ctx := openTrips(t)
	a, _ := s.OpenTrip(ctx)
	if _, err := s.CloseTrip(ctx, a.ID); err != nil {
		t.Fatal(err)
	}
	b, _ := s.OpenTrip(ctx)
	if _, err := s.CloseTrip(ctx, b.ID); err != nil {
		t.Fatal(err)
	}

	trips, next, err := s.ListTrips(ctx, 50, 0)
	if err != nil {
		t.Fatal(err)
	}
	if len(trips) != 2 || trips[0].ID != b.ID || trips[1].ID != a.ID || next != 0 {
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
