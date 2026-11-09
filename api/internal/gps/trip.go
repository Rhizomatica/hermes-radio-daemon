package gps

// The station's recorded trips (docs/API-REFERENCE.md, tag gps): open and
// close a trip, list them, and read one back with its breadcrumb points. A
// trip is opened explicitly and closed explicitly; closing does not open a
// new one, so at most one trip is open at a time.

import (
	"context"
	"database/sql"
	"errors"
	"net/http"
	"strconv"
	"time"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
)

func registerTrips(rt *api.Router, h *handlers) {
	rt.Op("openTrip", h.openTrip)
	rt.Op("closeTrip", h.closeTrip)
	rt.Op("getCurrentTrip", h.getCurrentTrip)
	rt.Op("listTrips", h.listTrips)
	rt.Op("getTrip", h.getTrip)
}

// Trip is Trip (api-rest.yaml).
type Trip struct {
	ID         int64   `json:"id"`
	StartedAt  string  `json:"started_at"`
	EndedAt    *string `json:"ended_at"`
	PointCount int     `json:"point_count"`
}

// GpsPoint is GpsPoint (api-rest.yaml).
type GpsPoint struct {
	At        string   `json:"at"`
	Latitude  float64  `json:"latitude"`
	Longitude float64  `json:"longitude"`
	AltitudeM *float64 `json:"altitude_m"`
	AccuracyM *float64 `json:"accuracy_m"`
}

func tripOf(t store.Trip) Trip {
	return Trip{ID: t.ID, StartedAt: t.StartedAt, EndedAt: t.EndedAt, PointCount: t.PointCount}
}

func pointOf(p store.TripPoint) GpsPoint {
	return GpsPoint{At: p.At, Latitude: p.Latitude, Longitude: p.Longitude,
		AltitudeM: p.AltitudeM, AccuracyM: p.AccuracyM}
}

func nextCursor(next int64) string {
	if next == 0 {
		return ""
	}
	return api.EncodeCursor(uint64(next))
}

// tripPage is getCurrentTrip's and getTrip's reply: the trip and one page of
// its points, oldest first.
type tripPage struct {
	Trip       Trip       `json:"trip"`
	Data       []GpsPoint `json:"data"`
	NextCursor string     `json:"next_cursor,omitempty"`
}

// dbFail answers 500 for a store error.
func dbFail(w http.ResponseWriter, r *http.Request) {
	api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
}

func (h *handlers) openTrip(w http.ResponseWriter, r *http.Request) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if _, ok, err := h.st.CurrentTrip(r.Context()); err != nil {
		dbFail(w, r)
		return
	} else if ok {
		api.Problem(w, r, http.StatusConflict, "trip_open", "a trip is already open; close it first", nil)
		return
	}
	trip, err := h.st.OpenTrip(r.Context())
	if err != nil {
		dbFail(w, r)
		return
	}
	h.startRecorder()
	api.JSON(w, http.StatusOK, tripOf(trip))
}

func (h *handlers) closeTrip(w http.ResponseWriter, r *http.Request) {
	h.mu.Lock()
	defer h.mu.Unlock()
	cur, ok, err := h.st.CurrentTrip(r.Context())
	if err != nil {
		dbFail(w, r)
		return
	}
	if !ok {
		api.Problem(w, r, http.StatusConflict, "no_open_trip", "no trip is open", nil)
		return
	}
	trip, err := h.st.CloseTrip(r.Context(), cur.ID)
	if err != nil {
		dbFail(w, r)
		return
	}
	h.stopRecorder()
	api.JSON(w, http.StatusOK, tripOf(trip))
}

func (h *handlers) getCurrentTrip(w http.ResponseWriter, r *http.Request) {
	cur, ok, err := h.st.CurrentTrip(r.Context())
	if err != nil {
		dbFail(w, r)
		return
	}
	if !ok {
		api.Problem(w, r, http.StatusNotFound, "no_open_trip", "no trip is open", nil)
		return
	}
	h.tripWithPoints(w, r, cur)
}

func (h *handlers) listTrips(w http.ResponseWriter, r *http.Request) {
	limit, ok := api.Limit(w, r)
	if !ok {
		return
	}
	cursor, ok := api.DecodeCursor(w, r)
	if !ok {
		return
	}
	trips, next, err := h.st.ListTrips(r.Context(), limit, int64(cursor))
	if err != nil {
		dbFail(w, r)
		return
	}
	data := make([]Trip, 0, len(trips))
	for _, t := range trips {
		data = append(data, tripOf(t))
	}
	api.JSON(w, http.StatusOK, api.Page[Trip]{Data: data, NextCursor: nextCursor(next)})
}

func (h *handlers) getTrip(w http.ResponseWriter, r *http.Request) {
	id, err := strconv.ParseInt(r.PathValue("trip_id"), 10, 64)
	if err != nil {
		api.Problem(w, r, http.StatusNotFound, "not_found", "no such trip", nil)
		return
	}
	trip, err := h.st.TripByID(r.Context(), id)
	if errors.Is(err, sql.ErrNoRows) {
		api.Problem(w, r, http.StatusNotFound, "not_found", "no such trip", nil)
		return
	}
	if err != nil {
		dbFail(w, r)
		return
	}
	h.tripWithPoints(w, r, trip)
}

func (h *handlers) tripWithPoints(w http.ResponseWriter, r *http.Request, trip store.Trip) {
	limit, ok := api.Limit(w, r)
	if !ok {
		return
	}
	cursor, ok := api.DecodeCursor(w, r)
	if !ok {
		return
	}
	points, next, err := h.st.TripPoints(r.Context(), trip.ID, limit, int64(cursor))
	if err != nil {
		dbFail(w, r)
		return
	}
	data := make([]GpsPoint, 0, len(points))
	for _, p := range points {
		data = append(data, pointOf(p))
	}
	api.JSON(w, http.StatusOK, tripPage{Trip: tripOf(trip), Data: data, NextCursor: nextCursor(next)})
}

// ── the recorder: one position fix per sample_interval_s into the open trip ──

func (h *handlers) startRecorder() {
	if h.recOn {
		return
	}
	h.recOn = true
	h.recStop = make(chan struct{})
	go h.recordLoop(h.recStop)
}

func (h *handlers) stopRecorder() {
	if !h.recOn {
		return
	}
	close(h.recStop)
	h.recOn = false
}

func (h *handlers) recordLoop(stop <-chan struct{}) {
	for {
		select {
		case <-stop:
			return
		case <-time.After(h.interval()):
			h.record()
		}
	}
}

// interval is the time between recorded fixes (GpsSettings sample_interval_s,
// at least one second).
func (h *handlers) interval() time.Duration {
	s := h.r.Gps().Settings.SampleIntervalS
	if s < 1 {
		s = 1
	}
	return time.Duration(s) * time.Second
}

func (h *handlers) record() {
	trip, ok, err := h.st.CurrentTrip(context.Background())
	if err != nil || !ok {
		return
	}
	g := h.r.Gps()
	if g.Latitude == nil || g.Longitude == nil {
		return
	}
	at := time.Now().UTC().Format(time.RFC3339)
	if g.At != nil {
		at = *g.At
	}
	_ = h.st.AddTripPoint(context.Background(), trip.ID, store.NewTripPoint{
		At: at, Latitude: *g.Latitude, Longitude: *g.Longitude, AltitudeM: g.AltitudeM,
	})
}
