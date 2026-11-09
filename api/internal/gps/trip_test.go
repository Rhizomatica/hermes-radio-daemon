package gps_test

import (
	"strconv"
	"testing"

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

	// Open.
	r := e.DoAuth("POST", "/gps/trip/open", "", token)
	if r.Status != 200 {
		t.Fatalf("open = %d %s", r.Status, r.Body)
	}
	j := r.JSON(t)
	id := tripID(t, j)
	if j["ended_at"] != nil || j["point_count"].(float64) != 0 || j["started_at"] == nil {
		t.Fatalf("open trip = %s", r.Body)
	}

	// A second open while one is open: 409 trip_open.
	if r := e.DoAuth("POST", "/gps/trip/open", "", token); r.Status != 409 || r.JSON(t)["code"] != "trip_open" {
		t.Fatalf("open while open = %d %s", r.Status, r.Body)
	}

	// The current trip and its (empty) breadcrumb.
	if r := e.DoAuth("GET", "/gps/trip", "", token); r.Status != 200 {
		t.Fatalf("GET /gps/trip = %d %s", r.Status, r.Body)
	} else {
		trip := r.JSON(t)["trip"].(map[string]any)
		if trip["id"].(float64) != j["id"].(float64) {
			t.Fatalf("current trip id = %v, want %v", trip["id"], id)
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
