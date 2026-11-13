// The station's GPS through the real daemon: GET /gps and PATCH
// /gps/settings (the Go API layer over the radiod C API), the hermes.v1 gps
// topic, and the fix read from gpsd's JSON socket.
//
// Copyright (C) 2026 Rhizomatica
// SPDX-License-Identifier: GPL-3.0-or-later

package integration

import (
	"bufio"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// Off by default (gps_enable = 0): GET /gps is the spec's defaults with no
// fix; PATCH /gps/settings applies, answers and persists to core.ini; a bad
// PATCH is refused and changes nothing.
func TestGPSRESTAndSettings(t *testing.T) {
	needGoAPI(t)
	d := startAPIDaemon(t)
	token := setupAPI(t, d)
	base := apiBase(d)

	r := callAuth(t, "GET", base+"/gps", "", token)
	if r.status != 200 {
		t.Fatalf("GET /gps = %d %s", r.status, r.raw)
	}
	if r.body["fix"] != "none" || r.body["latitude"] != nil || r.body["longitude"] != nil ||
		r.body["altitude_m"] != nil || r.body["at"] != nil {
		t.Errorf("GET /gps with gps disabled = %s", r.raw)
	}
	s, _ := r.body["settings"].(map[string]any)
	if s == nil {
		t.Fatalf("GET /gps settings = %s", r.raw)
	}
	if s["logging"] != false || s["sample_interval_s"].(float64) != 1 ||
		s["file_period_s"].(float64) != 3600 || s["email"] != nil {
		t.Errorf("default settings = %v", s)
	}
	if _, ok := s["report"]; ok {
		t.Errorf("settings still carry report (beacons are /gps/beacon's): %v", s)
	}

	r = callAuth(t, "PATCH", base+"/gps/settings",
		`{"logging":true,"sample_interval_s":5,"file_period_s":120,"email":"gps@example.com"}`, token)
	if r.status != 200 || r.body["logging"] != true || r.body["sample_interval_s"].(float64) != 5 ||
		r.body["file_period_s"].(float64) != 120 || r.body["email"] != "gps@example.com" {
		t.Fatalf("PATCH /gps/settings = %d %s", r.status, r.raw)
	}
	r = callAuth(t, "GET", base+"/gps", "", token)
	if r.status != 200 {
		t.Fatalf("GET /gps = %d %s", r.status, r.raw)
	}
	if s, _ := r.body["settings"].(map[string]any); s == nil || s["logging"] != true ||
		s["sample_interval_s"].(float64) != 5 || s["email"] != "gps@example.com" {
		t.Errorf("read back settings = %s", r.raw)
	}

	// A refused PATCH (one bad field) changes nothing.
	r = callAuth(t, "PATCH", base+"/gps/settings", `{"sample_interval_s":7,"file_period_s":1}`, token)
	if r.status != 422 || r.body["code"] != "invalid" {
		t.Fatalf("bad PATCH = %d %s", r.status, r.raw)
	}
	if r = callAuth(t, "GET", base+"/gps", "", token); r.status != 200 {
		t.Fatalf("GET /gps = %d %s", r.status, r.raw)
	}
	if s, _ := r.body["settings"].(map[string]any); s == nil || s["sample_interval_s"].(float64) != 5 {
		t.Errorf("a refused PATCH changed sample_interval_s: %s", r.raw)
	}

	d.Stop()

	b, err := os.ReadFile(filepath.Join(d.Dir, "core.ini"))
	if err != nil {
		t.Fatal(err)
	}
	// iniparser's dump pads the keys and quotes the values
	// (gps_logging                    = "1"), so read the values back with an
	// ini read rather than matching the raw lines.
	ini := parseINI(string(b))
	for _, kv := range []struct{ key, want string }{
		{"main:gps_logging", "1"},
		{"main:gps_sample_interval_s", "5"},
		{"main:gps_file_period_s", "120"},
		{"main:gps_email", "gps@example.com"},
	} {
		if got := ini[kv.key]; got != kv.want {
			t.Errorf("core.ini %s = %q, want %q\n%s", kv.key, got, kv.want, string(b))
		}
	}
}

// The hermes.v1 gps topic: subscribing gives the station's own Gps as the
// snapshot (change: self).
func TestGPSTopicSnapshot(t *testing.T) {
	needGoAPI(t)
	d := startAPIDaemon(t)
	token := setupAPI(t, d)
	ws, _, err := wsDialAuth(strings.TrimSuffix(d.WSURL, "/")+"/api/v2/ws", "hermes.v1", token)
	if err != nil {
		t.Fatal(err)
	}
	defer ws.Close()

	_ = readFrame(t, ws, 5*time.Second) // hello

	if err := ws.writeFrame(opText, []byte(`{"type":"subscribe","id":"s","topics":["gps"]}`)); err != nil {
		t.Fatal(err)
	}
	next := func(match func(map[string]any) bool) map[string]any {
		for {
			if f := readFrame(t, ws, 5*time.Second); match(f) {
				return f
			}
		}
	}
	// the snapshot comes before the reply
	snap := next(func(f map[string]any) bool { return f["type"] == "event" && f["topic"] == "gps" })
	if r := next(func(f map[string]any) bool { return f["type"] == "reply" }); r["ok"] != true {
		t.Fatalf("subscribe reply = %v", r)
	}
	if snap["snapshot"] != true || snap["seq"] != 0.0 {
		t.Errorf("gps snapshot envelope = %v", snap)
	}
	data, _ := snap["data"].(map[string]any)
	if data == nil || data["change"] != "self" || data["gps"] == nil {
		t.Errorf("gps snapshot = %v", snap)
	}
}

// With gps_enable and a fake gpsd, GET /gps reports the fix read from gpsd's
// JSON socket.
func TestGPSFixFromGpsd(t *testing.T) {
	needGoAPI(t)
	_, port, _ := net.SplitHostPort(startFakeGpsd(t))
	d := startDaemon(t, daemonOpts{core: []string{
		"api_v2_enable = 1",
		"gps_enable = 1",
		"gps_host = 127.0.0.1",
		"gps_port = " + port,
	}})
	token := setupAPI(t, d)

	var r httpResult
	if !eventually(10*time.Second, func() bool {
		r = callAuth(t, "GET", apiBase(d)+"/gps", "", token)
		return r.status == 200 && r.body["fix"] == "3d"
	}) {
		t.Fatalf("no 3-D fix within 10 s (last %d %s)\n--- daemon log ---\n%s", r.status, r.raw, d.Log())
	}
	lat, latOK := r.body["latitude"].(float64)
	lon, lonOK := r.body["longitude"].(float64)
	alt, altOK := r.body["altitude_m"].(float64)
	if !latOK || !lonOK || lat != 19.43 || lon != -99.13 {
		t.Errorf("fix position = %s", r.raw)
	}
	if !altOK || alt != 2240.0 || r.body["at"] == nil {
		t.Errorf("fix altitude/time = %s", r.raw)
	}
}

// Trips: open and close are explicit; closing does not open a new one. The
// trip appears in the list and is readable by id before and after closing.
func TestGPSTrips(t *testing.T) {
	needGoAPI(t)
	d := startAPIDaemon(t)
	token := setupAPI(t, d)
	base := apiBase(d)

	if r := callAuth(t, "GET", base+"/gps/trip", "", token); r.status != 404 || r.body["code"] != "no_open_trip" {
		t.Fatalf("GET /gps/trip = %d %s", r.status, r.raw)
	}

	r := callAuth(t, "POST", base+"/gps/trip/open", `{"name":"Field day"}`, token)
	if r.status != 200 {
		t.Fatalf("open = %d %s", r.status, r.raw)
	}
	id, _ := r.body["id"].(float64)
	if id == 0 || r.body["name"] != "Field day" || r.body["ended_at"] != nil || r.body["point_count"].(float64) != 0 {
		t.Fatalf("open trip = %s", r.raw)
	}

	if r := callAuth(t, "POST", base+"/gps/trip/open", `{"name":"Another"}`, token); r.status != 409 || r.body["code"] != "trip_open" {
		t.Fatalf("open while open = %d %s", r.status, r.raw)
	}

	r = callAuth(t, "GET", base+"/gps/trips", "", token)
	if r.status != 200 {
		t.Fatalf("list = %d %s", r.status, r.raw)
	}
	if data := r.body["data"].([]any); len(data) != 1 {
		t.Fatalf("list data = %v", r.body["data"])
	}

	// Closing does not open a new trip.
	r = callAuth(t, "POST", base+"/gps/trip/close", "", token)
	if r.status != 200 || r.body["ended_at"] == nil {
		t.Fatalf("close = %d %s", r.status, r.raw)
	}
	if r := callAuth(t, "GET", base+"/gps/trip", "", token); r.status != 404 {
		t.Fatalf("after close, GET /gps/trip = %d %s (close must not reopen)", r.status, r.raw)
	}

	// The closed trip is still in the history and readable by id.
	if r := callAuth(t, "GET", base+"/gps/trips/"+fmt.Sprintf("%.0f", id), "", token); r.status != 200 {
		t.Fatalf("get trip = %d %s", r.status, r.raw)
	}
	if r := callAuth(t, "GET", base+"/gps/trips/999999", "", token); r.status != 404 {
		t.Fatalf("get missing trip = %d %s", r.status, r.raw)
	}
}

// With gps_enable and a fake gpsd, an open trip records the fixes as its
// points (the breadcrumb). The fake moves, since the track only records a
// point when the station has moved (or its fix changed) or at its heartbeat.
// Also with the GPS time a day ahead of the system clock: the trip's bounds
// are on the track's clock, so it still finds its points.
func TestGPSTripRecording(t *testing.T) {
	needGoAPI(t)
	t.Run("same clock", func(t *testing.T) { tripRecording(t, 0) })
	t.Run("GPS a day ahead", func(t *testing.T) { tripRecording(t, 24*time.Hour) })
}

func tripRecording(t *testing.T, ahead time.Duration) {
	_, port, _ := net.SplitHostPort(startFakeGpsdAt(t, movingFix, ahead))
	d := startDaemon(t, daemonOpts{core: []string{
		"api_v2_enable = 1",
		"gps_enable = 1",
		"gps_host = 127.0.0.1",
		"gps_port = " + port,
		"gps_log_dir = " + t.TempDir(),
	}})
	token := setupAPI(t, d)
	base := apiBase(d)

	// Wait for a fix before opening the trip.
	var r httpResult
	if !eventually(10*time.Second, func() bool {
		r = callAuth(t, "GET", base+"/gps", "", token)
		return r.status == 200 && r.body["fix"] == "3d"
	}) {
		t.Fatalf("no 3-D fix within 10 s (last %d %s)", r.status, r.raw)
	}

	// Logging is off; opening the trip turns it on, so the track's first
	// point is in the trip.
	r = callAuth(t, "POST", base+"/gps/trip/open", `{"name":"Recording"}`, token)
	if r.status != 200 {
		t.Fatalf("open = %d %s", r.status, r.raw)
	}
	id := r.body["id"].(float64)
	started, err := time.Parse(time.RFC3339Nano, r.body["started_at"].(string))
	if want := time.Now().Add(ahead); err != nil || started.Sub(want).Abs() > time.Minute {
		t.Fatalf("started_at = %v, want about %v (the GPS time)", r.body["started_at"], want.UTC())
	}
	if r = callAuth(t, "GET", base+"/gps", "", token); r.status != 200 {
		t.Fatalf("GET /gps = %d %s", r.status, r.raw)
	} else if s, _ := r.body["settings"].(map[string]any); s == nil || s["logging"] != true {
		t.Fatalf("logging after open = %s", r.raw)
	}

	// The trip reads the track recorded while it is open.
	if !eventually(5*time.Second, func() bool {
		r = callAuth(t, "GET", base+"/gps/trip", "", token)
		if r.status != 200 {
			return false
		}
		trip, _ := r.body["trip"].(map[string]any)
		return trip != nil && trip["point_count"].(float64) >= 1
	}) {
		t.Fatalf("no recorded point within 5 s (last %d %s)\n--- daemon log ---\n%s", r.status, r.raw, d.Log())
	}
	data := r.body["data"].([]any)
	if len(data) == 0 {
		t.Fatalf("no points in the current trip: %s", r.raw)
	}
	pt := data[0].(map[string]any)
	if lat := pt["latitude"].(float64); (lat != 19.43 && lat != 19.4303) || pt["longitude"] != -99.13 {
		t.Fatalf("recorded point = %s", r.raw)
	}

	if r := callAuth(t, "POST", base+"/gps/trip/close", "", token); r.status != 200 {
		t.Fatalf("close = %d %s", r.status, r.raw)
	}
	if r := callAuth(t, "GET", base+"/gps/trips/"+fmt.Sprintf("%.0f", id), "", token); r.status != 200 {
		t.Fatalf("get trip = %d %s", r.status, r.raw)
	}
}

// startFakeGpsd runs a minimal gpsd on 127.0.0.1: it greets the reader and
// then streams a fixed 3-D TPV, so the daemon always has a fix waiting.
func startFakeGpsd(t *testing.T) string {
	t.Helper()
	return startFakeGpsdPath(t, nil)
}

// startFakeGpsdPath is startFakeGpsd whose n-th TPV is at path(n), so a test
// that needs the track to record points can make the station move.
func startFakeGpsdPath(t *testing.T, path func(n int) (lat, lon float64)) string {
	t.Helper()
	return startFakeGpsdAt(t, path, 0)
}

// startFakeGpsdAt is startFakeGpsdPath whose TPV times are ahead of the
// system clock by ahead, as a station's GPS time is when its clock is off.
func startFakeGpsdAt(t *testing.T, path func(n int) (lat, lon float64), ahead time.Duration) string {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { ln.Close() })
	go func() {
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			go serveFakeGpsd(conn, path, ahead)
		}
	}()
	return ln.Addr().String()
}

// movingFix is the station's position for the n-th TPV: two points ~33 m
// apart, so each sample interval's fix counts as a move and is recorded.
func movingFix(n int) (lat, lon float64) {
	if n%2 == 0 {
		return 19.43, -99.13
	}
	return 19.4303, -99.13
}

func serveFakeGpsd(conn net.Conn, path func(n int) (lat, lon float64), ahead time.Duration) {
	defer conn.Close()
	// The client's ?WATCH is fire-and-forget; drain it so the socket stays clear.
	go func() {
		br := bufio.NewReader(conn)
		for {
			if _, err := br.ReadString('\n'); err != nil {
				return
			}
		}
	}()
	if _, err := conn.Write([]byte(`{"class":"VERSION","release":"3.25","proto_major":3,"proto_minor":14}` + "\n")); err != nil {
		return
	}
	for n := 0; ; n++ {
		if _, err := conn.Write([]byte(fakeTPV(n, path, ahead))); err != nil {
			return
		}
		time.Sleep(250 * time.Millisecond)
	}
}

func fakeTPV(n int, path func(n int) (lat, lon float64), ahead time.Duration) string {
	lat, lon := 19.43, -99.13
	if path != nil {
		lat, lon = path(n)
	}
	// Sub-second precision: the track's times are milliseconds, and a whole
	// second rounds a fix down to before the trip that opened mid-second.
	at := time.Now().Add(ahead).UTC().Format(time.RFC3339Nano)
	return fmt.Sprintf(`{"class":"TPV","device":"fake","mode":3,"lat":%.7f,"lon":%.7f,"alt":2240.000,"altMSL":2240.000,"time":%q}`+"\n", lat, lon, at)
}

// parseINI reads a flat [section] key = value file into a map keyed by
// "section:key". Enough for the daemon's core.ini, whose values iniparser's
// dump writes with padded keys and quoted values.
func parseINI(content string) map[string]string {
	m := map[string]string{}
	section := ""
	for _, line := range strings.Split(content, "\n") {
		line = strings.TrimSpace(line)
		if line == "" || line[0] == ';' || line[0] == '#' {
			continue
		}
		if line[0] == '[' {
			if end := strings.IndexByte(line, ']'); end > 0 {
				section = line[1:end]
			}
			continue
		}
		eq := strings.IndexByte(line, '=')
		if eq < 0 {
			continue
		}
		key := strings.TrimSpace(line[:eq])
		val := strings.Trim(strings.TrimSpace(line[eq+1:]), `"`)
		m[section+":"+key] = val
	}
	return m
}
