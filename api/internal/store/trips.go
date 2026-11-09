package store

import (
	"context"
	"database/sql"
	"fmt"
)

// Trip is one row of the trips table. EndedAt is nil while the trip is open.
type Trip struct {
	ID         int64
	StartedAt  string
	EndedAt    *string
	PointCount int
}

// TripPoint is one row of the trip_points table.
type TripPoint struct {
	ID        int64
	At        string
	Latitude  float64
	Longitude float64
	AltitudeM *float64
	AccuracyM *float64
}

// NewTripPoint is what AddTripPoint needs.
type NewTripPoint struct {
	At        string
	Latitude  float64
	Longitude float64
	AltitudeM *float64
	AccuracyM *float64
}

const tripCols = "id, started_at, ended_at"

// OpenTrip inserts a new open trip (ended_at null) and returns it.
func (s *Store) OpenTrip(ctx context.Context) (Trip, error) {
	res, err := s.db.ExecContext(ctx, `INSERT INTO trips DEFAULT VALUES`)
	if err != nil {
		return Trip{}, fmt.Errorf("open trip: %w", err)
	}
	id, err := res.LastInsertId()
	if err != nil {
		return Trip{}, err
	}
	return s.TripByID(ctx, id)
}

// CurrentTrip returns the open trip (ended_at null), ok false when none is
// open. With at most one open trip, the highest id is the open one.
func (s *Store) CurrentTrip(ctx context.Context) (Trip, bool, error) {
	t, err := scanTrip(s.db.QueryRowContext(ctx,
		`SELECT `+tripCols+` FROM trips WHERE ended_at IS NULL ORDER BY id DESC LIMIT 1`))
	if err == sql.ErrNoRows {
		return Trip{}, false, nil
	}
	if err != nil {
		return Trip{}, false, err
	}
	t.PointCount, err = s.pointCount(ctx, t.ID)
	return t, true, err
}

// CloseTrip sets ended_at on the open trip with id, and returns it.
func (s *Store) CloseTrip(ctx context.Context, id int64) (Trip, error) {
	res, err := s.db.ExecContext(ctx,
		`UPDATE trips SET ended_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now') WHERE id = ? AND ended_at IS NULL`, id)
	if err != nil {
		return Trip{}, fmt.Errorf("close trip %d: %w", id, err)
	}
	if n, err := res.RowsAffected(); err == nil && n == 0 {
		return Trip{}, sql.ErrNoRows
	}
	return s.TripByID(ctx, id)
}

// TripByID returns one trip, or sql.ErrNoRows.
func (s *Store) TripByID(ctx context.Context, id int64) (Trip, error) {
	t, err := scanTrip(s.db.QueryRowContext(ctx,
		`SELECT `+tripCols+` FROM trips WHERE id = ?`, id))
	if err != nil {
		return Trip{}, err
	}
	t.PointCount, err = s.pointCount(ctx, id)
	return t, err
}

// ListTrips returns one page of trips, newest first, and the id to use as the
// next cursor (0 on the last page).
func (s *Store) ListTrips(ctx context.Context, limit int, cursor int64) ([]Trip, int64, error) {
	var (
		rows *sql.Rows
		err  error
	)
	if cursor > 0 {
		rows, err = s.db.QueryContext(ctx,
			`SELECT `+tripCols+` FROM trips WHERE id < ? ORDER BY id DESC LIMIT ?`, cursor, limit+1)
	} else {
		rows, err = s.db.QueryContext(ctx,
			`SELECT `+tripCols+` FROM trips ORDER BY id DESC LIMIT ?`, limit+1)
	}
	if err != nil {
		return nil, 0, err
	}
	defer rows.Close()
	var out []Trip
	for rows.Next() {
		t, err := scanTrip(rows)
		if err != nil {
			return nil, 0, err
		}
		out = append(out, t)
	}
	if err := rows.Err(); err != nil {
		return nil, 0, err
	}
	for i := range out {
		if out[i].PointCount, err = s.pointCount(ctx, out[i].ID); err != nil {
			return nil, 0, err
		}
	}
	// Fetch one extra to know whether another page follows.
	var next int64
	if len(out) > limit {
		out = out[:limit]
		next = out[len(out)-1].ID
	}
	return out, next, nil
}

// AddTripPoint inserts one position point into a trip.
func (s *Store) AddTripPoint(ctx context.Context, tripID int64, p NewTripPoint) error {
	_, err := s.db.ExecContext(ctx, `
		INSERT INTO trip_points (trip_id, at, latitude, longitude, altitude_m, accuracy_m)
		VALUES (?, ?, ?, ?, ?, ?)`,
		tripID, p.At, p.Latitude, p.Longitude, p.AltitudeM, p.AccuracyM)
	if err != nil {
		return fmt.Errorf("add trip point to %d: %w", tripID, err)
	}
	return nil
}

// TripPoints returns one page of a trip's points, oldest first, and the point
// id to use as the next cursor (0 on the last page).
func (s *Store) TripPoints(ctx context.Context, tripID int64, limit int, cursor int64) ([]TripPoint, int64, error) {
	rows, err := s.db.QueryContext(ctx, `
		SELECT id, at, latitude, longitude, altitude_m, accuracy_m
		FROM trip_points WHERE trip_id = ? AND id > ? ORDER BY id ASC LIMIT ?`,
		tripID, cursor, limit+1)
	if err != nil {
		return nil, 0, err
	}
	defer rows.Close()
	var out []TripPoint
	for rows.Next() {
		p, err := scanTripPoint(rows)
		if err != nil {
			return nil, 0, err
		}
		out = append(out, p)
	}
	if err := rows.Err(); err != nil {
		return nil, 0, err
	}
	// Fetch one extra to know whether another page follows.
	var next int64
	if len(out) > limit {
		out = out[:limit]
		next = out[len(out)-1].ID
	}
	return out, next, nil
}

func (s *Store) pointCount(ctx context.Context, tripID int64) (int, error) {
	var n int
	err := s.db.QueryRowContext(ctx,
		`SELECT count(*) FROM trip_points WHERE trip_id = ?`, tripID).Scan(&n)
	return n, err
}

func scanTrip(sc interface{ Scan(...any) error }) (Trip, error) {
	var t Trip
	var ended sql.NullString
	if err := sc.Scan(&t.ID, &t.StartedAt, &ended); err != nil {
		return Trip{}, err
	}
	t.EndedAt = ns(ended)
	return t, nil
}

func scanTripPoint(sc interface{ Scan(...any) error }) (TripPoint, error) {
	var p TripPoint
	var alt, acc sql.NullFloat64
	if err := sc.Scan(&p.ID, &p.At, &p.Latitude, &p.Longitude, &alt, &acc); err != nil {
		return TripPoint{}, err
	}
	if alt.Valid {
		p.AltitudeM = &alt.Float64
	}
	if acc.Valid {
		p.AccuracyM = &acc.Float64
	}
	return p, nil
}
