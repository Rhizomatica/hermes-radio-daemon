package gps

// The position beacon (docs/BEACON-FORMAT.md): its settings, sending one,
// the emergency code (SOS), the stations heard, and those stations as the
// map's station list.

import (
	"bufio"
	"encoding/json/jsontext"
	"encoding/json/v2"
	"errors"
	"net/http"
	"os"
	"regexp"
	"strconv"
	"strings"
	"time"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
)

const (
	// SosDefault is sendSos's code without one: distress, unspecified.
	SosDefault = 1
	// SosMax is the highest code a station sets; SosOver is "emergency
	// over", sent by clearing it.
	SosMax  = 30
	SosOver = 31
	// IntervalMin is the shortest interval between regular beacons, and
	// EmergencyIntervalMin between those of an emergency (beacon.h).
	IntervalMin          = 60
	EmergencyIntervalMin = 30
	// LatMax and LonMax bound a position.
	LatMax = 90
	LonMax = 180
)

// The beacon's fix types (beacon.h BEACON_FIX_*).
const (
	fixNone = iota
	fix2D
	fix3D
	fixManual
)

// UUCPSys is UUCP's list of the stations this one calls: the stations the
// map lists (listStationPositions). A variable for the tests.
var UUCPSys = "/etc/uucp/sys"

// callsignRe is BeaconPatch's callsign (the beacon's alphabet, no space).
var callsignRe = regexp.MustCompile(`^([A-Za-z0-9/.-]{1,9})?$`)

func registerBeacon(rt *api.Router, h handlers) {
	rt.Op("getBeacon", h.getBeacon)
	rt.Op("updateBeacon", h.updateBeacon)
	rt.Op("sendBeacon", h.sendBeacon)
	rt.Op("sendSos", h.sendSos)
	rt.Op("clearSos", h.clearSos)
	rt.Op("listBeacons", h.listBeacons)
	rt.Op("listStationPositions", h.listStations)
}

// Beacon is Beacon.
type Beacon struct {
	Enabled            bool     `json:"enabled"`
	IntervalS          int      `json:"interval_s"`
	Callsign           string   `json:"callsign"`
	StationLatitude    *float64 `json:"station_latitude"`
	StationLongitude   *float64 `json:"station_longitude"`
	EmergencyIntervalS int      `json:"emergency_interval_s"`
	Emergency          int      `json:"emergency"`
	Running            bool     `json:"running"`
	SosRecipients      []string `json:"sos_recipients"`
}

func beaconOf(b rad.Beacon) Beacon {
	return Beacon{
		Enabled: b.Enabled, IntervalS: b.IntervalS,
		Callsign: b.Callsign, StationLatitude: b.StationLatitude, StationLongitude: b.StationLongitude,
		EmergencyIntervalS: b.EmergencyIntervalS, Emergency: b.Emergency, Running: b.Running,
		SosRecipients: append([]string{}, b.SosRecipients...),
	}
}

func (h handlers) getBeacon(w http.ResponseWriter, r *http.Request) {
	api.JSON(w, http.StatusOK, beaconOf(h.r.Beacon()))
}

// updateBeacon checks every field before it applies any. The position is
// both coordinates or neither (null clears it).
func (h handlers) updateBeacon(w http.ResponseWriter, r *http.Request) {
	var raw map[string]jsontext.Value
	if !api.Decode(w, r, &raw) {
		return
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	next := h.r.Beacon()
	f := api.Fields{}
	for key, v := range raw {
		switch key {
		case "enabled":
			var e bool
			if json.Unmarshal(v, &e) != nil {
				f.Add(key, "a boolean")
			} else {
				next.Enabled = e
			}
		case "interval_s":
			if n, ok := intIn(v, key, IntervalMin, 1<<31-1, f); ok {
				next.IntervalS = int(n)
			}
		case "callsign":
			var c string
			if json.Unmarshal(v, &c) != nil || !callsignRe.MatchString(c) {
				f.Add(key, "up to 9 of A-Z 0-9 / . - (empty: no beacons)")
			} else {
				next.Callsign = strings.ToUpper(c)
			}
		case "station_latitude", "station_longitude":
			max := LatMax
			if key == "station_longitude" {
				max = LonMax
			}
			if strings.TrimSpace(string(v)) == "null" {
				if key == "station_latitude" {
					next.StationLatitude = nil
				} else {
					next.StationLongitude = nil
				}
				continue
			}
			var x float64
			if json.Unmarshal(v, &x) != nil || x < -float64(max) || x > float64(max) {
				f.Add(key, "degrees, within ±"+strconv.Itoa(max))
			} else if key == "station_latitude" {
				next.StationLatitude = &x
			} else {
				next.StationLongitude = &x
			}
		case "emergency_interval_s":
			if n, ok := intIn(v, key, EmergencyIntervalMin, 1<<31-1, f); ok {
				next.EmergencyIntervalS = int(n)
			}
		case "sos_recipients":
			if list, ok := h.recipients(v, key, f); ok {
				next.SosRecipients = list
			}
		default:
			f.Add(key, "not a beacon setting")
		}
	}
	if (next.StationLatitude == nil) != (next.StationLongitude == nil) {
		f.Add("station_latitude", "both coordinates, or neither")
	}
	if len(f) > 0 {
		api.Invalid(w, r, f)
		return
	}
	if err := h.r.SetBeacon(next); err != nil {
		api.Invalid(w, r, api.Fields{"beacon": {err.Error()}})
		return
	}
	api.JSON(w, http.StatusOK, beaconOf(h.r.Beacon()))
}

// down answers 503 for ErrDown and reports whether err was nil.
func down(w http.ResponseWriter, r *http.Request, err error) bool {
	switch {
	case err == nil:
		return true
	case errors.Is(err, rad.ErrDown):
		api.Problem(w, r, http.StatusServiceUnavailable, "not_running",
			"the broadcast plane is not running: no beacon goes out (main:broadcast_enable)", nil)
	case errors.Is(err, rad.ErrInvalid):
		api.Invalid(w, r, api.Fields{"code": {"1 to " + strconv.Itoa(SosMax)}})
	default:
		api.Problem(w, r, http.StatusInternalServerError, "internal", err.Error(), nil)
	}
	return false
}

func (h handlers) sendBeacon(w http.ResponseWriter, r *http.Request) {
	if down(w, r, h.r.BeaconSendNow()) {
		p, _ := api.PrincipalFrom(r)
		h.log.Info("radio.tx beacon", "user", p.UserID)
		w.WriteHeader(http.StatusAccepted)
	}
}

func (h handlers) sendSos(w http.ResponseWriter, r *http.Request) {
	req := struct {
		Code *int `json:"code"`
	}{}
	if r.ContentLength != 0 && !api.Decode(w, r, &req) {
		return
	}
	code := SosDefault
	if req.Code != nil {
		code = *req.Code
	}
	if code < 1 || code > SosMax {
		api.Invalid(w, r, api.Fields{"code": {"1 to " + strconv.Itoa(SosMax)}})
		return
	}
	h.sos.set.Lock()
	defer h.sos.set.Unlock()
	if !h.sosGoesOut(w, r) {
		return
	}
	if down(w, r, h.r.SetBeaconEmergency(code)) {
		p, _ := api.PrincipalFrom(r)
		h.log.Warn("radio.tx emergency beacon", "user", p.UserID, "code", code)
		go h.sosQueue(code, time.Now())
		api.JSON(w, http.StatusAccepted, beaconOf(h.r.Beacon()))
	}
}

// sosGoesOut is whether an SOS reaches anyone: on the air (the broadcast
// plane runs) or to sos_recipients over UUCP or NNCP. False after
// answering 503 when neither.
func (h handlers) sosGoesOut(w http.ResponseWriter, r *http.Request) bool {
	if b := h.r.Beacon(); b.Running || len(b.SosRecipients) > 0 {
		return true
	}
	return down(w, r, rad.ErrDown)
}

func (h handlers) clearSos(w http.ResponseWriter, r *http.Request) {
	// The read and the clear are one step: of two clears at once, only the
	// first finds an emergency, so one "emergency over" goes out.
	h.sos.set.Lock()
	defer h.sos.set.Unlock()
	if !h.sosGoesOut(w, r) {
		return
	}
	was := h.r.Beacon().Emergency
	if down(w, r, h.r.SetBeaconEmergency(0)) {
		p, _ := api.PrincipalFrom(r)
		h.log.Warn("radio.tx emergency over", "user", p.UserID)
		if was != 0 {
			go h.sosQueue(SosOver, time.Now())
		}
		api.JSON(w, http.StatusOK, beaconOf(h.r.Beacon()))
	}
}

// Heard is BeaconHeard.
type Heard struct {
	Callsign  string  `json:"callsign"`
	Latitude  float64 `json:"latitude"`
	Longitude float64 `json:"longitude"`
	AltitudeM *int    `json:"altitude_m"`
	Emergency int     `json:"emergency"`
	Fix       int     `json:"fix"`
	HeardAt   string  `json:"heard_at"`
}

func (h handlers) listBeacons(w http.ResponseWriter, r *http.Request) {
	st := h.r.BeaconStations()
	out := make([]Heard, 0, len(st))
	for _, s := range st {
		out = append(out, Heard{s.Callsign, s.Latitude, s.Longitude, s.AltitudeM, s.Emergency, s.Fix, s.HeardAt})
	}
	api.JSON(w, http.StatusOK, map[string]any{"data": out})
}

// Station is Station, the parts this station knows: a UUCP system and its
// last-known position from its beacons (callsign = UUCP name).
type Station struct {
	UucpName       string   `json:"uucp_name"`
	Enabled        bool     `json:"enabled"`
	Latitude       *float64 `json:"latitude"`
	Longitude      *float64 `json:"longitude"`
	AltitudeM      *float64 `json:"altitude_m"`
	PositionAt     *string  `json:"position_at"`
	PositionSource *string  `json:"position_source"`
}

// uucpSystems are the "system <name>" entries of UUCP's sys file, in order.
func uucpSystems(path string) []string {
	f, err := os.Open(path)
	if err != nil {
		return nil
	}
	defer f.Close()
	var out []string
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		line := sc.Text()
		if i := strings.IndexByte(line, '#'); i >= 0 {
			line = line[:i]
		}
		fs := strings.Fields(line)
		if len(fs) >= 2 && fs[0] == "system" {
			out = append(out, fs[1])
		}
	}
	return out
}

func sourceOf(fix int) *string {
	var s string
	switch fix {
	case fix2D, fix3D:
		s = "gps"
	case fixManual:
		s = "manual"
	default:
		return nil
	}
	return &s
}

func (h handlers) listStations(w http.ResponseWriter, r *http.Request) {
	heard := map[string]rad.BeaconStation{}
	for _, s := range h.r.BeaconStations() { // newest first: keep the first
		k := strings.ToUpper(s.Callsign)
		if _, ok := heard[k]; !ok {
			heard[k] = s
		}
	}
	out := []Station{}
	for _, name := range uucpSystems(UUCPSys) {
		st := Station{UucpName: name, Enabled: true}
		if b, ok := heard[strings.ToUpper(name)]; ok {
			lat, lon, at := b.Latitude, b.Longitude, b.HeardAt
			st.Latitude, st.Longitude, st.PositionAt = &lat, &lon, &at
			if b.AltitudeM != nil {
				a := float64(*b.AltitudeM)
				st.AltitudeM = &a
			}
			st.PositionSource = sourceOf(b.Fix)
		}
		out = append(out, st)
	}
	api.JSON(w, http.StatusOK, map[string]any{"data": out})
}
