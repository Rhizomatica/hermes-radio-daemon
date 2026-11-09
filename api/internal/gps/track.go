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
)

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
	pts, err := r.GpsTrack(station, since.UnixMilli(), until.UnixMilli(), limit+1)
	if err != nil {
		return nil, time.Time{}, err
	}
	var next time.Time
	if len(pts) > limit {
		pts = pts[:limit]
		next = time.UnixMilli(pts[limit-1].AtMs + 1) // a track's times only go forward
	}
	out := make([]Point, len(pts))
	for i, p := range pts {
		out[i] = pointOf(p)
	}
	return out, next, nil
}

// Count is how many points a track has at times since..until, without
// reading them (a trip's point_count). rad.ErrInvalid as Track.
func Count(r rad.Radio, station string, since, until time.Time) (int, error) {
	return r.GpsTrackCount(station, since.UnixMilli(), until.UnixMilli())
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
	from, ok := api.DecodeCursor(w, r) // the time the page starts at, unix ms
	if !ok {
		return
	}
	if from != 0 {
		since = time.UnixMilli(int64(from))
	}
	pts, next, err := Track(h.r, station, since, until, limit)
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
		page.NextCursor = api.EncodeCursor(uint64(next.UnixMilli()))
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

func (h handlers) listFiles(w http.ResponseWriter, r *http.Request) {
	out := []File{}
	for _, p := range h.r.GpsTrackFiles() {
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
	for _, p := range h.r.GpsTrackFiles() {
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

func (h handlers) deleteFiles(w http.ResponseWriter, r *http.Request) {
	for _, p := range h.r.GpsTrackFiles() {
		// ErrInvalid: trimmed since it was listed
		if err := h.r.GpsTrackDelete(filepath.Base(p)); err != nil && !errors.Is(err, rad.ErrInvalid) {
			api.Problem(w, r, http.StatusInternalServerError, "internal", "cannot delete "+filepath.Base(p), nil)
			return
		}
	}
	w.WriteHeader(http.StatusNoContent)
}
