// Package gps serves the station's GPS REST operations (api-rest.yaml, tag
// gps) and the hermes.v1 gps topic, from the fix radiod reads through libgps.
package gps

import (
	"encoding/json"
	"fmt"
	"net/http"
	"strings"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/spec"
)

// Register adds the GPS operations and the gps topic.
func Register(rt *api.Router) {
	h := handlers{rt.Radio()}
	rt.Op("getGps", h.getGps)
	rt.Op("updateGpsSettings", h.updateSettings)

	// The station's own Gps, pushed as it changes (change: self). Other
	// stations' positions (change: peers) are not there yet.
	rt.Topic("gps", spec.RoleReadonly, func() (any, error) {
		return map[string]any{"change": "self", "gps": gpsOf(h.r.Gps())}, nil
	})
}

type handlers struct{ r rad.Radio }

// ── response schemas (api-rest.yaml components) ──

// Gps is Gps.
type Gps struct {
	Fix       string   `json:"fix"`
	Latitude  *float64 `json:"latitude"`
	Longitude *float64 `json:"longitude"`
	AltitudeM *float64 `json:"altitude_m"`
	At        *string  `json:"at"`
	Settings  Settings `json:"settings"`
}

// Settings is GpsSettings.
type Settings struct {
	Logging         bool    `json:"logging"`
	SampleIntervalS int     `json:"sample_interval_s"`
	FilePeriodS     int     `json:"file_period_s"`
	Email           *string `json:"email"`
	Report          Report  `json:"report"`
}

// Report is the periodic over-the-air position report.
type Report struct {
	Enabled   bool `json:"enabled"`
	IntervalS int  `json:"interval_s"`
}

func gpsOf(g rad.Gps) Gps {
	return Gps{
		Fix: g.Fix, Latitude: g.Latitude, Longitude: g.Longitude,
		AltitudeM: g.AltitudeM, At: g.At, Settings: settingsOf(g.Settings),
	}
}

func settingsOf(s rad.GpsSettings) Settings {
	return Settings{
		Logging: s.Logging, SampleIntervalS: s.SampleIntervalS,
		FilePeriodS: s.FilePeriodS, Email: s.Email,
		Report: Report{Enabled: s.Report.Enabled, IntervalS: s.Report.IntervalS},
	}
}

// ── REST ──

func (h handlers) getGps(w http.ResponseWriter, r *http.Request) {
	api.JSON(w, http.StatusOK, gpsOf(h.r.Gps()))
}

// updateSettings checks every field before it applies any, then answers with
// the settings as they are.
func (h handlers) updateSettings(w http.ResponseWriter, r *http.Request) {
	var raw map[string]json.RawMessage
	if !api.Decode(w, r, &raw) {
		return
	}
	cur := h.r.Gps().Settings
	f := api.Fields{}
	next := cur
	for key := range raw {
		switch key {
		case "logging":
			var b bool
			if err := json.Unmarshal(raw[key], &b); err != nil {
				f.Add(key, "a boolean")
			} else {
				next.Logging = b
			}
		case "sample_interval_s":
			if v, ok := intIn(raw[key], key, 1, 1<<31-1, f); ok {
				next.SampleIntervalS = int(v)
			}
		case "file_period_s":
			if v, ok := intIn(raw[key], key, 60, 1<<31-1, f); ok {
				next.FilePeriodS = int(v)
			}
		case "email":
			if strings.TrimSpace(string(raw[key])) == "null" {
				next.Email = nil
			} else {
				var e string
				if json.Unmarshal(raw[key], &e) != nil || !validEmail(e) {
					f.Add(key, "an email address, or null")
				} else {
					next.Email = &e
				}
			}
		case "report":
			if rp, ok := parseReport(raw[key], key, cur.Report, f); ok {
				next.Report = rp
			}
		default:
			f.Add(key, "unknown field")
		}
	}
	if !f.OK() {
		api.Invalid(w, r, f)
		return
	}
	if err := h.r.SetGpsSettings(next); err != nil {
		h.problemFor(w, r, err)
		return
	}
	api.JSON(w, http.StatusOK, settingsOf(h.r.Gps().Settings))
}

func (h handlers) problemFor(w http.ResponseWriter, r *http.Request, err error) {
	switch err {
	case rad.ErrInvalid, rad.ErrNotSupported:
		api.Problem(w, r, http.StatusUnprocessableEntity, "invalid", err.Error(), nil)
	default:
		api.Problem(w, r, http.StatusInternalServerError, "internal", err.Error(), nil)
	}
}

// parseReport checks and merges the report object over the current one; both
// fields are optional.
func parseReport(raw json.RawMessage, key string, cur rad.GpsReport, f api.Fields) (rad.GpsReport, bool) {
	if strings.TrimSpace(string(raw)) == "null" {
		f.Add(key, "an object, not null")
		return cur, false
	}
	var m map[string]json.RawMessage
	if err := json.Unmarshal(raw, &m); err != nil {
		f.Add(key, "an object with enabled and interval_s")
		return cur, false
	}
	rp := cur
	for k := range m {
		switch k {
		case "enabled":
			var b bool
			if err := json.Unmarshal(m[k], &b); err != nil {
				f.Add(key+".enabled", "a boolean")
			} else {
				rp.Enabled = b
			}
		case "interval_s":
			if v, ok := intIn(m[k], key+".interval_s", 60, 1<<31-1, f); ok {
				rp.IntervalS = int(v)
			}
		default:
			f.Add(key+"."+k, "unknown field")
		}
	}
	return rp, true
}

func intIn(raw json.RawMessage, key string, lo, hi int64, f api.Fields) (int64, bool) {
	var v int64
	if err := json.Unmarshal(raw, &v); err != nil {
		f.Add(key, "an integer")
		return 0, false
	}
	if v < lo || v > hi {
		f.Add(key, fmt.Sprintf("from %d to %d", lo, hi))
		return 0, false
	}
	return v, true
}

// validEmail is a light check; the C layer's buffer is the hard cap (128).
func validEmail(e string) bool {
	return len(e) > 0 && len(e) <= 128 && strings.Contains(e, "@") && !strings.ContainsAny(e, " \t\r\n")
}
