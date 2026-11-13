package gps

// The station's recorded trips (docs/API-REFERENCE.md, tag gps): open and
// close a trip, list them, and read one back with its breadcrumb points. A
// trip is a named time range of the station's track (track.go), opened
// explicitly and closed explicitly; closing does not open a new one, so at
// most one trip is open at a time.

import (
	"database/sql"
	"errors"
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
)

const (
	// tripNameMax is a trip name's length in characters (api-rest.yaml
	// Trip.name maxLength): enough for a label in a list or on the map.
	tripNameMax = 64
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
	ID        int64   `json:"id"`
	Name      string  `json:"name"`
	StartedAt string  `json:"started_at"`
	EndedAt   *string `json:"ended_at"`
	// PointCount is absent only when the track could not be read just after
	// opening or closing the trip: the trip changed, so the reply is still 200.
	PointCount *int `json:"point_count,omitempty"`
}

// tripOf is a trip with its point count (nil: left out).
func tripOf(t store.Trip, pointCount *int) Trip {
	return Trip{ID: t.ID, Name: t.Name, StartedAt: t.StartedAt, EndedAt: t.EndedAt, PointCount: pointCount}
}

// trackNow is now on the track's clock, the one its points are stamped
// with: the GPS time while radiod has had a GPS fix, the system clock
// before. A trip's bounds take the same clock, so on a station whose system
// clock is off the trip still covers its own points.
func (h *handlers) trackNow() time.Time {
	return time.UnixMilli(h.r.GpsClockMs())
}

// tripTimes is a trip's time range: started_at, and ended_at or now on the
// track's clock while it is open.
func (h *handlers) tripTimes(t store.Trip) (since, until time.Time, err error) {
	if since, err = time.Parse(time.RFC3339Nano, t.StartedAt); err != nil {
		return time.Time{}, time.Time{}, err
	}
	if t.EndedAt == nil {
		return since, h.trackNow(), nil
	}
	until, err = time.Parse(time.RFC3339Nano, *t.EndedAt)
	return since, until, err
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

// dbFail logs a store error and answers 500.
func (h *handlers) dbFail(w http.ResponseWriter, r *http.Request, err error) {
	h.log.Error("gps trip: database", "op", r.Pattern, "err", err)
	api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
}

// trackFail logs a track read error (or a stored time that won't parse) and
// answers 500.
func (h *handlers) trackFail(w http.ResponseWriter, r *http.Request, err error) {
	h.log.Error("gps trip: track", "op", r.Pattern, "err", err)
	api.Problem(w, r, http.StatusInternalServerError, "internal", "the track could not be read", nil)
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
	case utf8.RuneCountInString(name) > tripNameMax:
		f.Add("name", fmt.Sprintf("at most %d characters", tripNameMax))
	}
	if !f.OK() {
		api.Invalid(w, r, f)
		return
	}

	// The trips' own mutex, not h.mu: a slow database write must not delay
	// the beacon and emergency sends that h.mu guards.
	h.tripMu.Lock()
	defer h.tripMu.Unlock()

	// Refuse before touching logging, so a refused open changes nothing. The
	// schema's index still refuses a trip another process opened meanwhile.
	if _, open, err := h.st.CurrentTrip(r.Context()); err != nil {
		h.dbFail(w, r, err)
		return
	} else if open {
		api.Problem(w, r, http.StatusConflict, "trip_open", "a trip is already open; close it first", nil)
		return
	}

	// A trip's points are the track's, so the track must be recording.
	wasLogging, err := h.setLogging(true)
	if err != nil {
		h.problemFor(w, r, err)
		return
	}
	trip, err := h.st.OpenTrip(r.Context(), name, h.trackNow())
	if err != nil {
		// A refused open changes nothing: put logging back as it was.
		if !wasLogging {
			if _, rerr := h.setLogging(false); rerr != nil {
				h.log.Warn("gps trip: logging left on after a failed open", "err", rerr)
			}
		}
		if errors.Is(err, store.ErrTripOpen) {
			api.Problem(w, r, http.StatusConflict, "trip_open", "a trip is already open; close it first", nil)
		} else {
			h.dbFail(w, r, err)
		}
		return
	}
	api.JSON(w, http.StatusOK, h.tripReply(trip))
}

// setLogging turns the track's position logging on or off and says whether
// it was on. Under h.mu, as updateSettings, so the two don't drop each
// other's change.
func (h *handlers) setLogging(on bool) (was bool, err error) {
	h.mu.Lock()
	defer h.mu.Unlock()
	s := h.r.Gps().Settings
	if s.Logging == on {
		return on, nil
	}
	was = s.Logging
	s.Logging = on
	return was, h.r.SetGpsSettings(s)
}

// tripReply is a trip just opened or closed, with its point count. The trip
// has changed by then, so a track that can't be read leaves the count out
// instead of failing the call.
func (h *handlers) tripReply(trip store.Trip) Trip {
	count, err := h.pointCount(trip)
	if err != nil {
		h.log.Warn("gps trip: no point count", "trip", trip.ID, "err", err)
		return tripOf(trip, nil)
	}
	return tripOf(trip, &count)
}

func (h *handlers) closeTrip(w http.ResponseWriter, r *http.Request) {
	h.tripMu.Lock()
	defer h.tripMu.Unlock()
	cur, ok, err := h.st.CurrentTrip(r.Context())
	if err != nil {
		h.dbFail(w, r, err)
		return
	}
	if !ok {
		api.Problem(w, r, http.StatusConflict, "no_open_trip", "no trip is open", nil)
		return
	}
	// Not before the start: the clock moves when the first fix corrects it.
	end := h.trackNow()
	if since, err := time.Parse(time.RFC3339Nano, cur.StartedAt); err == nil && end.Before(since) {
		end = since
	}
	trip, err := h.st.CloseTrip(r.Context(), cur.ID, end)
	if errors.Is(err, sql.ErrNoRows) {
		// Another process closed it between the read and the update.
		api.Problem(w, r, http.StatusConflict, "no_open_trip", "no trip is open", nil)
		return
	}
	if err != nil {
		h.dbFail(w, r, err)
		return
	}
	api.JSON(w, http.StatusOK, h.tripReply(trip))
}

func (h *handlers) getCurrentTrip(w http.ResponseWriter, r *http.Request) {
	cur, ok, err := h.st.CurrentTrip(r.Context())
	if err != nil {
		h.dbFail(w, r, err)
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
		h.dbFail(w, r, err)
		return
	}
	// Every trip's point count in one pass over the track's files.
	since, until := make([]time.Time, len(trips)), make([]time.Time, len(trips))
	for i, t := range trips {
		if since[i], until[i], err = h.tripTimes(t); err != nil {
			h.trackFail(w, r, err)
			return
		}
	}
	counts, err := Counts(h.r, "", since, until)
	if err != nil {
		h.trackFail(w, r, err)
		return
	}
	data := make([]Trip, len(trips))
	for i, t := range trips {
		data[i] = tripOf(t, &counts[i])
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
		h.dbFail(w, r, err)
		return
	}
	h.tripWithPoints(w, r, trip)
}

func (h *handlers) pointCount(t store.Trip) (int, error) {
	since, until, err := h.tripTimes(t)
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
	since, until, err := h.tripTimes(trip)
	if err != nil {
		h.trackFail(w, r, err)
		return
	}
	count, err := Count(h.r, "", since, until)
	if err != nil {
		h.trackFail(w, r, err)
		return
	}
	from, skip := since, 0
	if cursor != 0 {
		from, skip = decodeTrackCursor(cursor)
	}
	// A page's cursor must not reach outside the trip's range.
	if from.Before(since) {
		from, skip = since, 0
	}
	points, next, nextSkip, err := trackFrom(h.r, "", from, until, skip, limit)
	if err != nil {
		h.trackFail(w, r, err)
		return
	}
	page := tripPage{Trip: tripOf(trip, &count), Data: points}
	if !next.IsZero() {
		page.NextCursor = encodeTrackCursor(next, nextSkip)
	}
	api.JSON(w, http.StatusOK, page)
}
