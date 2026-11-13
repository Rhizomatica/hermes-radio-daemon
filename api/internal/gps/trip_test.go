package gps_test

import (
	"net/url"
	"strconv"
	"strings"
	"testing"
	"time"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
)

// tripID returns the string form of a trip's id from a decoded reply.
func tripID(t *testing.T, j map[string]any) string {
	t.Helper()
	id, ok := j["id"].(float64)
	if !ok {
		t.Fatalf("no numeric id in %v", j)
	}
	return strconv.FormatInt(int64(id), 10)
}

func openTrip(t *testing.T, e *testapi.Env, token, name string) map[string]any {
	t.Helper()
	r := e.DoAuth("POST", "/gps/trip/open", `{"name":"`+name+`"}`, token)
	if r.Status != 200 {
		t.Fatalf("open = %d %s", r.Status, r.Body)
	}
	return r.JSON(t)
}

func TestTripLifecycle(t *testing.T) {
	e := testapi.NewAuth(t)
	e.SeedUser("admin", "password", "admin")
	token := e.Login("admin", "password")

	// No trip is open yet.
	if r := e.DoAuth("GET", "/gps/trip", "", token); r.Status != 404 || r.JSON(t)["code"] != "no_open_trip" {
		t.Fatalf("GET /gps/trip = %d %s", r.Status, r.Body)
	}
	if r := e.DoAuth("POST", "/gps/trip/close", "", token); r.Status != 409 || r.JSON(t)["code"] != "no_open_trip" {
		t.Fatalf("close with none open = %d %s", r.Status, r.Body)
	}

	// Opening needs a non-empty name.
	if r := e.DoAuth("POST", "/gps/trip/open", `{}`, token); r.Status != 422 {
		t.Fatalf("open without a name = %d %s", r.Status, r.Body)
	}
	if r := e.DoAuth("POST", "/gps/trip/open", `{"name":"  "}`, token); r.Status != 422 {
		t.Fatalf("open with a blank name = %d %s", r.Status, r.Body)
	}

	// Open.
	r := e.DoAuth("POST", "/gps/trip/open", `{"name":"Field day"}`, token)
	if r.Status != 200 {
		t.Fatalf("open = %d %s", r.Status, r.Body)
	}
	j := r.JSON(t)
	id := tripID(t, j)
	if j["name"] != "Field day" || j["ended_at"] != nil || j["point_count"].(float64) != 0 || j["started_at"] == nil {
		t.Fatalf("open trip = %s", r.Body)
	}

	// A second open while one is open: 409 trip_open.
	if r := e.DoAuth("POST", "/gps/trip/open", `{"name":"Another"}`, token); r.Status != 409 || r.JSON(t)["code"] != "trip_open" {
		t.Fatalf("open while open = %d %s", r.Status, r.Body)
	}

	// The current trip and its (empty) breadcrumb.
	if r := e.DoAuth("GET", "/gps/trip", "", token); r.Status != 200 {
		t.Fatalf("GET /gps/trip = %d %s", r.Status, r.Body)
	} else {
		trip := r.JSON(t)["trip"].(map[string]any)
		if trip["id"].(float64) != j["id"].(float64) || trip["name"] != "Field day" {
			t.Fatalf("current trip = %v, want %v", trip, id)
		}
	}

	// List has the one trip.
	if r := e.DoAuth("GET", "/gps/trips", "", token); r.Status != 200 {
		t.Fatalf("list = %d %s", r.Status, r.Body)
	} else if data := r.JSON(t)["data"].([]any); len(data) != 1 {
		t.Fatalf("list data = %v", r.JSON(t)["data"])
	}

	// Get by id, and a missing id.
	if r := e.DoAuth("GET", "/gps/trips/"+id, "", token); r.Status != 200 {
		t.Fatalf("get = %d %s", r.Status, r.Body)
	}
	if r := e.DoAuth("GET", "/gps/trips/999999", "", token); r.Status != 404 {
		t.Fatalf("get missing = %d %s", r.Status, r.Body)
	}

	// Close; closing does not open a new trip.
	if r := e.DoAuth("POST", "/gps/trip/close", "", token); r.Status != 200 || r.JSON(t)["ended_at"] == nil {
		t.Fatalf("close = %d %s", r.Status, r.Body)
	}
	if r := e.DoAuth("GET", "/gps/trip", "", token); r.Status != 404 {
		t.Fatalf("after close, GET /gps/trip = %d %s (close must not reopen)", r.Status, r.Body)
	}
	if r := e.DoAuth("POST", "/gps/trip/close", "", token); r.Status != 409 {
		t.Fatalf("close again = %d %s", r.Status, r.Body)
	}
}

// A trip's points and point_count are the station's track in its time range,
// not a second recording.
func TestTripReadsTrack(t *testing.T) {
	e := testapi.NewAuth(t)
	e.SeedUser("admin", "password", "admin")
	token := e.Login("admin", "password")

	j := openTrip(t, e, token, "Track trip")
	start, err := time.Parse(time.RFC3339Nano, j["started_at"].(string))
	if err != nil {
		t.Fatalf("started_at = %v: %v", j["started_at"], err)
	}
	at := start.UnixMilli()
	e.Radio.SetGpsTrack("",
		rad.GpsPoint{AtMs: at, LatE7: 194300000, LonE7: -991300000, AltDm: 22400, AccuracyDm: 35, Fix: rad.GpsFix3D},
		rad.GpsPoint{AtMs: at, LatE7: 194300001, LonE7: -991300001, AltDm: rad.GpsNoAlt, AccuracyDm: rad.GpsNoAcc, Fix: rad.GpsFix2D, Emergency: 4},
	)

	r := e.DoAuth("GET", "/gps/trip", "", token)
	if r.Status != 200 {
		t.Fatalf("GET /gps/trip = %d %s", r.Status, r.Body)
	}
	j = r.JSON(t)
	trip := j["trip"].(map[string]any)
	if trip["point_count"].(float64) != 2 {
		t.Fatalf("point_count = %v, want 2 (body %s)", trip["point_count"], r.Body)
	}
	data := j["data"].([]any)
	if len(data) != 2 {
		t.Fatalf("data = %v", data)
	}
	p := data[1].(map[string]any)
	if p["latitude"].(float64) != 19.4300001 || p["longitude"].(float64) != -99.1300001 ||
		p["fix"] != "2d" || p["emergency"].(float64) != 4 || p["altitude_m"] != nil {
		t.Fatalf("point = %v", p)
	}

	// The list counts the same range.
	r = e.DoAuth("GET", "/gps/trips", "", token)
	if r.Status != 200 {
		t.Fatalf("list = %d %s", r.Status, r.Body)
	}
	if got := r.JSON(t)["data"].([]any)[0].(map[string]any)["point_count"].(float64); got != 2 {
		t.Fatalf("list point_count = %v, want 2", got)
	}
}

// A trip's points page like the track's: points that share a timestamp are
// neither skipped nor repeated when a page boundary falls inside that run.
func TestTripPagesSameTime(t *testing.T) {
	e := testapi.NewAuth(t)
	e.SeedUser("admin", "password", "admin")
	token := e.Login("admin", "password")

	open := openTrip(t, e, token, "Same time")
	start, err := time.Parse(time.RFC3339Nano, open["started_at"].(string))
	if err != nil {
		t.Fatalf("started_at = %v: %v", open["started_at"], err)
	}
	at := start.UnixMilli()
	pts := make([]rad.GpsPoint, 3)
	for i := range pts {
		pts[i] = rad.GpsPoint{AtMs: at, LatE7: int32(194300000 + i)}
	}
	e.Radio.SetGpsTrack("", pts...)

	q := "/gps/trips/" + tripID(t, open) + "?limit=1"
	r := e.DoAuth("GET", q, "", token)
	if r.Status != 200 {
		t.Fatalf("get = %d %s", r.Status, r.Body)
	}
	j := r.JSON(t)
	var got []float64
	for {
		for _, p := range j["data"].([]any) {
			got = append(got, p.(map[string]any)["latitude"].(float64))
		}
		next, _ := j["next_cursor"].(string)
		if next == "" {
			break
		}
		r = e.DoAuth("GET", q+"&cursor="+url.QueryEscape(next), "", token)
		if r.Status != 200 {
			t.Fatalf("page = %d %s", r.Status, r.Body)
		}
		j = r.JSON(t)
	}
	if len(got) != len(pts) {
		t.Fatalf("points over the pages = %v, want %d", got, len(pts))
	}
	for i, lat := range got {
		if want := float64(194300000+i) / 1e7; lat != want {
			t.Errorf("point %d = %v, want %v", i, lat, want)
		}
	}
}

func TestTripTurnsLoggingOn(t *testing.T) {
	e := testapi.NewAuth(t)
	e.SeedUser("admin", "password", "admin")
	token := e.Login("admin", "password")
	logging := func() bool { return e.Radio.Gps().Settings.Logging }

	if logging() {
		t.Fatal("the stub starts with logging on")
	}
	trip := openTrip(t, e, token, "Field day")
	if !logging() {
		t.Fatal("opening a trip left logging off")
	}

	// A refused open changes nothing, logging included.
	if r := e.DoAuth("PATCH", "/gps/settings", `{"logging":false}`, token); r.Status != 200 {
		t.Fatalf("logging off = %d %s", r.Status, r.Body)
	}
	if r := e.DoAuth("POST", "/gps/trip/open", `{"name":"Another"}`, token); r.Status != 409 {
		t.Fatalf("open while open = %d %s", r.Status, r.Body)
	}
	if logging() {
		t.Fatal("a refused open turned logging on")
	}

	// Closing leaves logging as it is.
	if r := e.DoAuth("PATCH", "/gps/settings", `{"logging":true}`, token); r.Status != 200 {
		t.Fatalf("logging on = %d %s", r.Status, r.Body)
	}
	if r := e.DoAuth("POST", "/gps/trip/close", "", token); r.Status != 200 || r.JSON(t)["id"] != trip["id"] {
		t.Fatalf("close = %d %s", r.Status, r.Body)
	}
	if !logging() {
		t.Fatal("closing a trip turned logging off")
	}
}

// A trip's bounds are on the track's clock, the one its points are stamped
// with (the GPS time), not the system clock, so a station whose system clock
// is off still finds its points.
func TestTripUsesTheTrackClock(t *testing.T) {
	e := testapi.NewAuth(t)
	e.SeedUser("admin", "password", "admin")
	token := e.Login("admin", "password")

	base := time.Date(2030, 1, 1, 0, 0, 0, 0, time.UTC)
	e.Radio.SetGpsClock(base)
	trip := openTrip(t, e, token, "Clock")
	if s, _ := trip["started_at"].(string); !strings.HasPrefix(s, "2030-01-01T00:00:00.") {
		t.Fatalf("started_at = %v, want the track's clock", trip["started_at"])
	}
	e.Radio.SetGpsTrack("", rad.GpsPoint{AtMs: base.Add(500 * time.Millisecond).UnixMilli(), Fix: rad.GpsFix3D})
	e.Radio.SetGpsClock(base.Add(time.Second))

	if r := e.DoAuth("GET", "/gps/trip", "", token); r.Status != 200 || len(r.JSON(t)["data"].([]any)) != 1 {
		t.Fatalf("open trip points = %d %s", r.Status, r.Body)
	}
	r := e.DoAuth("POST", "/gps/trip/close", "", token)
	j := r.JSON(t)
	if s, _ := j["ended_at"].(string); r.Status != 200 || !strings.HasPrefix(s, "2030-01-01T00:00:01.") || j["point_count"].(float64) != 1 {
		t.Fatalf("close = %d %s", r.Status, r.Body)
	}
}

// A clock that moves back while a trip is open (a first fix correcting a
// system clock that was ahead) closes the trip at its start, never before.
func TestTripCloseNotBeforeStart(t *testing.T) {
	e := testapi.NewAuth(t)
	e.SeedUser("admin", "password", "admin")
	token := e.Login("admin", "password")

	e.Radio.SetGpsClock(time.Date(2030, 1, 1, 0, 0, 0, 0, time.UTC))
	trip := openTrip(t, e, token, "Back")
	e.Radio.SetGpsClock(time.Date(2029, 1, 1, 0, 0, 0, 0, time.UTC))
	r := e.DoAuth("POST", "/gps/trip/close", "", token)
	if j := r.JSON(t); r.Status != 200 || j["ended_at"] != trip["started_at"] {
		t.Fatalf("close = %d %s, want ended_at %v", r.Status, r.Body, trip["started_at"])
	}
}

func TestTripNameLength(t *testing.T) {
	e := testapi.NewAuth(t)
	e.SeedUser("admin", "password", "admin")
	token := e.Login("admin", "password")

	// The limit counts characters, not bytes.
	long := strings.Repeat("ç", 64)
	if r := e.DoAuth("POST", "/gps/trip/open", `{"name":"`+long+`"}`, token); r.Status != 200 {
		t.Fatalf("64 characters = %d %s", r.Status, r.Body)
	}
	if r := e.DoAuth("POST", "/gps/trip/close", "", token); r.Status != 200 {
		t.Fatalf("close = %d %s", r.Status, r.Body)
	}
	if r := e.DoAuth("POST", "/gps/trip/open", `{"name":"`+long+`ç"}`, token); r.Status != 422 {
		t.Fatalf("65 characters = %d %s", r.Status, r.Body)
	}
}

// The trip list counts every trip's points in one pass over the track.
func TestTripListCountsInOnePass(t *testing.T) {
	e := testapi.NewAuth(t)
	e.SeedUser("admin", "password", "admin")
	token := e.Login("admin", "password")

	base := time.Date(2030, 1, 1, 0, 0, 0, 0, time.UTC)
	var pts []rad.GpsPoint
	for i := 0; i < 3; i++ {
		e.Radio.SetGpsClock(base.Add(time.Duration(10*i) * time.Minute))
		openTrip(t, e, token, "trip "+strconv.Itoa(i))
		for k := 0; k <= i; k++ {
			at := base.Add(time.Duration(10*i)*time.Minute + time.Duration(k+1)*time.Second)
			pts = append(pts, rad.GpsPoint{AtMs: at.UnixMilli(), Fix: rad.GpsFix3D})
		}
		e.Radio.SetGpsClock(base.Add(time.Duration(10*i+5) * time.Minute))
		if r := e.DoAuth("POST", "/gps/trip/close", "", token); r.Status != 200 {
			t.Fatalf("close = %d %s", r.Status, r.Body)
		}
	}
	e.Radio.SetGpsTrack("", pts...)

	before := e.Radio.TrackCountCalls()
	r := e.DoAuth("GET", "/gps/trips", "", token)
	if r.Status != 200 {
		t.Fatalf("list = %d %s", r.Status, r.Body)
	}
	data := r.JSON(t)["data"].([]any)
	for i, want := range []float64{3, 2, 1} { // newest first
		if got := data[i].(map[string]any)["point_count"]; got != want {
			t.Errorf("trip %d point_count = %v, want %v", i, got, want)
		}
	}
	if calls := e.Radio.TrackCountCalls() - before; calls != 1 {
		t.Errorf("track count passes = %d, want 1", calls)
	}
}
