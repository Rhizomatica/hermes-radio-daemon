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
	if rep, _ := s["report"].(map[string]any); rep["enabled"] != false || rep["interval_s"].(float64) != 900 {
		t.Errorf("default report = %v", rep)
	}

	r = callAuth(t, "PATCH", base+"/gps/settings",
		`{"logging":true,"sample_interval_s":5,"file_period_s":120,"email":"gps@example.com","report":{"enabled":true,"interval_s":300}}`, token)
	if r.status != 200 || r.body["logging"] != true || r.body["sample_interval_s"].(float64) != 5 ||
		r.body["file_period_s"].(float64) != 120 || r.body["email"] != "gps@example.com" {
		t.Fatalf("PATCH /gps/settings = %d %s", r.status, r.raw)
	}
	if rep, _ := r.body["report"].(map[string]any); rep["enabled"] != true || rep["interval_s"].(float64) != 300 {
		t.Errorf("report = %v", rep)
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
		{"main:gps_report_enabled", "1"},
		{"main:gps_report_interval_s", "300"},
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

// startFakeGpsd runs a minimal gpsd on 127.0.0.1: it greets the reader and
// then streams a fixed 3-D TPV, so the daemon always has a fix waiting.
func startFakeGpsd(t *testing.T) string {
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
			go serveFakeGpsd(conn)
		}
	}()
	return ln.Addr().String()
}

func serveFakeGpsd(conn net.Conn) {
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
	for {
		if _, err := conn.Write([]byte(fakeTPV())); err != nil {
			return
		}
		time.Sleep(250 * time.Millisecond)
	}
}

func fakeTPV() string {
	at := time.Now().UTC().Format(time.RFC3339)
	return fmt.Sprintf(`{"class":"TPV","device":"fake","mode":3,"lat":19.430000,"lon":-99.130000,"alt":2240.000,"altMSL":2240.000,"time":%q}`+"\n", at)
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
