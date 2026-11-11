package gps

// The station's recorded trips (docs/API-REFERENCE.md, tag gps): open and
// close a trip, list them, and read one back with its breadcrumb points. A
// trip is a named time range of the station's track (track.go), opened
// explicitly and closed explicitly; closing does not open a new one, so at
// most one trip is open at a time.

import (
	"database/sql"
	"errors"
	"net/http"
	"strconv"
	"strings"
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
	Name       string  `json:"name"`
	StartedAt  string  `json:"started_at"`
	EndedAt    *string `json:"ended_at"`
	PointCount int     `json:"point_count"`
}

func tripOf(t store.Trip, pointCount int) Trip {
	return Trip{ID: t.ID, Name: t.Name, StartedAt: t.StartedAt, EndedAt: t.EndedAt, PointCount: pointCount}
}

// tripTimes is a trip's time range: started_at, and ended_at or now while it
// is open.
func tripTimes(t store.Trip) (since, until time.Time, err error) {
	if since, err = time.Parse(time.RFC3339Nano, t.StartedAt); err != nil {
		return time.Time{}, time.Time{}, err
	}
	if t.EndedAt == nil {
		return since, time.Now(), nil
	}
	until, err = time.Parse(time.RFC3339Nano, *t.EndedAt)
	return since, until, err
}

func nextCursor(next time.Time) string {
	if next.IsZero() {
		return ""
	}
	return api.EncodeCursor(uint64(next.UnixMilli()))
}

func idCursor(next int64) string {
	if next == 0 {
		return ""
	}
	return api.EncodeCursor(uint64(next))
}

// tripPage is getCurrentTrip's and getTrip's reply: the trip and one page of
// its points, oldest first.
type tripPage struct {
	Trip       Trip    `json:"trip"`
	Data       []Point `json:"data"`
	NextCursor string  `json:"next_cursor,omitempty"`
}

// dbFail answers 500 for a store error.
func dbFail(w http.ResponseWriter, r *http.Request) {
	api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
}

// trackFail answers 500 for a track read error.
func trackFail(w http.ResponseWriter, r *http.Request, err error) {
	api.Problem(w, r, http.StatusInternalServerError, "internal", err.Error(), nil)
}

func (h *handlers) openTrip(w http.ResponseWriter, r *http.Request) {
	var in struct {
		Name string `json:"name"`
	}
	if !api.Decode(w, r, &in) {
		return
	}
	name := strings.TrimSpace(in.Name)
	f := api.Fields{}
	switch {
	case name == "":
		f.Add("name", "a non-empty name")
	case len(name) > 64:
		f.Add("name", "at most 64 characters")
	}
	if !f.OK() {
		api.Invalid(w, r, f)
		return
	}

	// The trips' own mutex, not h.mu: a slow database write must not delay
	// the beacon and emergency sends that h.mu guards.
	h.tripMu.Lock()
	defer h.tripMu.Unlock()
	if _, ok, err := h.st.CurrentTrip(r.Context()); err != nil {
		dbFail(w, r)
		return
	} else if ok {
		api.Problem(w, r, http.StatusConflict, "trip_open", "a trip is already open; close it first", nil)
		return
	}
	trip, err := h.st.OpenTrip(r.Context(), name)
	if errors.Is(err, store.ErrTripOpen) {
		api.Problem(w, r, http.StatusConflict, "trip_open", "a trip is already open; close it first", nil)
		return
	}
	if err != nil {
		dbFail(w, r)
		return
	}
	api.JSON(w, http.StatusOK, tripOf(trip, 0))
}

func (h *handlers) closeTrip(w http.ResponseWriter, r *http.Request) {
	h.tripMu.Lock()
	defer h.tripMu.Unlock()
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
	count, err := h.pointCount(trip)
	if err != nil {
		trackFail(w, r, err)
		return
	}
	api.JSON(w, http.StatusOK, tripOf(trip, count))
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
		count, err := h.pointCount(t)
		if err != nil {
			trackFail(w, r, err)
			return
		}
		data = append(data, tripOf(t, count))
	}
	api.JSON(w, http.StatusOK, api.Page[Trip]{Data: data, NextCursor: idCursor(next)})
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

func (h *handlers) pointCount(t store.Trip) (int, error) {
	since, until, err := tripTimes(t)
	if err != nil {
		return 0, err
	}
	return Count(h.r, "", since, until)
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
	since, until, err := tripTimes(trip)
	if err != nil {
		trackFail(w, r, err)
		return
	}
	count, err := Count(h.r, "", since, until)
	if err != nil {
		trackFail(w, r, err)
		return
	}
	from := since
	if cursor != 0 {
		from = time.UnixMilli(int64(cursor))
	}
	points, next, err := Track(h.r, "", from, until, limit)
	if err != nil {
		trackFail(w, r, err)
		return
	}
	api.JSON(w, http.StatusOK, tripPage{Trip: tripOf(trip, count), Data: points, NextCursor: nextCursor(next)})
}
