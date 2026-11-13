package gps

// Station tracks (gps/gps_track.h): this station's positions while logging,
// and every position beacon heard from another station, read by time
// range. Track and Count are the reader trips are built on: a trip is a
// time range of a track.

import (
	"errors"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
)

const (
	trackSpanDefault  = 24 * time.Hour // getTrack's since when not given: a day back
	trackLimitDefault = 1000
	trackLimitMax     = 5000 // a page of about 650 KB of JSON
	callsignMax       = 15   // gps_track.h GPS_TRACK_CALL_MAX
	e7                = 1e7  // a record's 1e-7 degrees
	dmPerM            = 10.0 // a record's decimetres
	pointTimeLayout   = "2006-01-02T15:04:05.000Z07:00"
	trackFilePrefix   = "gps-" // gps_track.h: gps-YYYYMMDDTHHMMSSZ.trk
	trackFileStamp    = "20060102T150405Z"

	// A track page's cursor packs the last point's time and how many points
	// sharing that time were already returned. A track's times repeat
	// (simultaneous fixes share a millisecond), so the time alone is not a
	// position: without the count the next page would skip or repeat them.
	// The count is 16 bits, far more than a millisecond can hold, and caps
	// what a crafted cursor can make the reader allocate.
	trackCursorSkipBits = 16
	trackCursorSkipMask = 1<<trackCursorSkipBits - 1
)

// errTrackRun is a run of points sharing one millisecond longer than a page
// cursor's skip count can step over (trackCursorSkipBits): the next page
// would repeat points, so paging stops with this error instead.
var errTrackRun = errors.New("too many track points share one time to page through")

// Point is GpsPoint.
type Point struct {
	At        string   `json:"at"`
	Latitude  float64  `json:"latitude"`
	Longitude float64  `json:"longitude"`
	AltitudeM *float64 `json:"altitude_m"`
	AccuracyM *float64 `json:"accuracy_m"`
	Fix       string   `json:"fix"`
	Emergency int      `json:"emergency"`
}

func pointOf(p rad.GpsPoint) Point {
	out := Point{
		At:        time.UnixMilli(p.AtMs).UTC().Format(pointTimeLayout),
		Latitude:  float64(p.LatE7) / e7,
		Longitude: float64(p.LonE7) / e7,
		Emergency: int(p.Emergency),
	}
	if p.AltDm != rad.GpsNoAlt {
		a := float64(p.AltDm) / dmPerM
		out.AltitudeM = &a
	}
	if p.AccuracyDm != rad.GpsNoAcc {
		a := float64(p.AccuracyDm) / dmPerM
		out.AccuracyM = &a
	}
	switch p.Fix {
	case rad.GpsFix2D:
		out.Fix = "2d"
	case rad.GpsFix3D:
		out.Fix = "3d"
	case rad.GpsFixManual:
		out.Fix = "manual"
	default:
		out.Fix = "none"
	}
	return out
}

// Track reads up to limit points of a track (station "": this station's;
// else a callsign) at times since..until, both included, oldest first, and
// the time the next page starts at (zero when there are no more).
// rad.ErrInvalid when station is no callsign.
func Track(r rad.Radio, station string, since, until time.Time, limit int) ([]Point, time.Time, error) {
	pts, next, _, err := trackFrom(r, station, since, until, 0, limit)
	return pts, next, err
}

// trackFrom is Track starting after the first skip points at exactly since
// (the points a previous page already returned from that time). It also
// returns how many of the page's trailing points share its last time, which
// becomes the next page's skip. A zero next time means there are no more.
func trackFrom(r rad.Radio, station string, since, until time.Time, skip, limit int) ([]Point, time.Time, int, error) {
	if limit <= 0 {
		return []Point{}, time.Time{}, 0, nil
	}
	at := since.UnixMilli()
	// Fetch the skipped points too, so a full page of new points follows them.
	pts, err := r.GpsTrack(station, at, until.UnixMilli(), limit+1+skip)
	if err != nil {
		return nil, time.Time{}, 0, err
	}
	for i := 0; i < skip && len(pts) > 0 && pts[0].AtMs == at; i++ {
		pts = pts[1:]
	}
	var next time.Time
	nextSkip := 0
	if len(pts) > limit {
		pts = pts[:limit]
		last := pts[limit-1].AtMs
		next = time.UnixMilli(last)
		for i := len(pts) - 1; i >= 0 && pts[i].AtMs == last; i-- {
			nextSkip++
		}
		if last == at {
			nextSkip += skip // still in the run the previous page returned
		}
		if nextSkip > trackCursorSkipMask {
			return nil, time.Time{}, 0, errTrackRun
		}
	}
	out := make([]Point, len(pts))
	for i, p := range pts {
		out[i] = pointOf(p)
	}
	return out, next, nextSkip, nil
}

// encodeTrackCursor and decodeTrackCursor make a track page position opaque.
func encodeTrackCursor(at time.Time, skip int) string {
	return api.EncodeCursor(uint64(at.UnixMilli())<<trackCursorSkipBits | uint64(skip&trackCursorSkipMask))
}

func decodeTrackCursor(v uint64) (time.Time, int) {
	return time.UnixMilli(int64(v >> trackCursorSkipBits)), int(v & trackCursorSkipMask)
}

// Count is how many points a track has at times since..until, without
// reading them (a trip's point_count). rad.ErrInvalid as Track.
func Count(r rad.Radio, station string, since, until time.Time) (int, error) {
	return r.GpsTrackCount(station, since.UnixMilli(), until.UnixMilli())
}

// Counts is Count for many ranges at once, in one pass over the track's
// files (a page of trips' point_count).
func Counts(r rad.Radio, station string, since, until []time.Time) ([]int, error) {
	s, u := make([]int64, len(since)), make([]int64, len(until))
	for i := range since {
		s[i] = since[i].UnixMilli()
	}
	for i := range until {
		u[i] = until[i].UnixMilli()
	}
	return r.GpsTrackCounts(station, s, u)
}

func (h handlers) getTrack(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	f := api.Fields{}
	now := time.Now()
	since, until := now.Add(-trackSpanDefault), now
	for key, dst := range map[string]*time.Time{"since": &since, "until": &until} {
		if s := q.Get(key); s != "" {
			t, err := time.Parse(time.RFC3339, s)
			if err != nil {
				f.Add(key, "an ISO 8601 date-time")
			}
			*dst = t
		}
	}
	station := q.Get("callsign")
	if len(station) > callsignMax {
		f.Add("callsign", fmt.Sprintf("at most %d characters", callsignMax))
	}
	limit := trackLimitDefault
	if s := q.Get("limit"); s != "" {
		n, err := strconv.Atoi(s)
		if err != nil || n < 1 || n > trackLimitMax {
			f.Add("limit", fmt.Sprintf("an integer from 1 to %d", trackLimitMax))
		}
		limit = n
	}
	if f.OK() && until.Before(since) {
		f.Add("until", "not before since")
	}
	if !f.OK() {
		api.Invalid(w, r, f)
		return
	}
	from, ok := api.DecodeCursor(w, r) // the previous page's last position
	if !ok {
		return
	}
	skip := 0
	if from != 0 {
		// A cursor never reaches before since: an old or foreign one would
		// otherwise read the track from 1970.
		if at, n := decodeTrackCursor(from); !at.Before(since) {
			since, skip = at, n
		}
	}
	pts, next, nextSkip, err := trackFrom(h.r, station, since, until, skip, limit)
	if errors.Is(err, rad.ErrInvalid) {
		api.Invalid(w, r, api.Fields{"callsign": {"a callsign: letters, digits, '-', '/' and '.'"}})
		return
	}
	if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", err.Error(), nil)
		return
	}
	page := api.Page[Point]{Data: pts}
	if !next.IsZero() {
		page.NextCursor = encodeTrackCursor(next, nextSkip)
	}
	api.JSON(w, http.StatusOK, page)
}

// ── /gps/files: this station's track files ──

// File is GpsFile.
type File struct {
	Name      string `json:"name"`
	SizeBytes int64  `json:"size_bytes"`
	CreatedAt string `json:"created_at"`
}

// periodStart is a track file's period start, from its name.
func periodStart(name string) string {
	stamp := strings.TrimSuffix(strings.TrimPrefix(name, trackFilePrefix), filepath.Ext(name))
	t, err := time.Parse(trackFileStamp, stamp)
	if err != nil {
		return ""
	}
	return t.UTC().Format(time.RFC3339)
}

// trackFiles is this station's track files, or false after answering 500.
func (h handlers) trackFiles(w http.ResponseWriter, r *http.Request) ([]string, bool) {
	paths, err := h.r.GpsTrackFiles()
	if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", err.Error(), nil)
		return nil, false
	}
	return paths, true
}

func (h handlers) listFiles(w http.ResponseWriter, r *http.Request) {
	paths, ok := h.trackFiles(w, r)
	if !ok {
		return
	}
	out := []File{}
	for _, p := range paths {
		fi, err := os.Stat(p)
		if err != nil {
			continue // trimmed since it was listed
		}
		name := filepath.Base(p)
		out = append(out, File{Name: name, SizeBytes: fi.Size(), CreatedAt: periodStart(name)})
	}
	api.JSON(w, http.StatusOK, api.Page[File]{Data: out})
}

func (h handlers) downloadFile(w http.ResponseWriter, r *http.Request) {
	name := r.PathValue("name")
	paths, ok := h.trackFiles(w, r)
	if !ok {
		return
	}
	for _, p := range paths {
		if filepath.Base(p) != name {
			continue
		}
		f, err := os.Open(p)
		if err != nil {
			break
		}
		defer f.Close()
		fi, err := f.Stat()
		if err != nil {
			break
		}
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Header().Set("Content-Disposition", `attachment; filename="`+name+`"`)
		http.ServeContent(w, r, name, fi.ModTime(), f) // ranges: a download resumed over a slow link
		return
	}
	api.Problem(w, r, http.StatusNotFound, "not_found", "no such position file", nil)
}

// deleteFiles deletes every file: a list holds the newest 65536, so it lists
// again until none are left.
func (h handlers) deleteFiles(w http.ResponseWriter, r *http.Request) {
	for {
		paths, ok := h.trackFiles(w, r)
		if !ok {
			return
		}
		deleted := 0
		for _, p := range paths {
			err := h.r.GpsTrackDelete(filepath.Base(p))
			switch {
			case err == nil:
				deleted++
			case errors.Is(err, rad.ErrNotFound): // trimmed since it was listed
			default:
				api.Problem(w, r, http.StatusInternalServerError, "internal", "cannot delete "+filepath.Base(p)+": "+err.Error(), nil)
				return
			}
		}
		if deleted == 0 { // none left, or only ones already gone
			break
		}
	}
	w.WriteHeader(http.StatusNoContent)
}
