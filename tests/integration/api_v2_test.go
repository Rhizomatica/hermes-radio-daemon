// The /api/v2 REST API and the hermes.v1 websocket (api, Go,
// linked into radiod as a C archive) through the real daemon.
//
// Copyright (C) 2026 Rhizomatica
// SPDX-License-Identifier: GPL-3.0-or-later

package integration

import (
	"debug/elf"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"testing"
	"time"
)

// hasGoAPI reports whether the binary has the Go API layer linked in.
func hasGoAPI(t *testing.T) bool {
	t.Helper()
	f, err := elf.Open(radiodBin)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	syms, _ := f.Symbols()
	for _, s := range syms {
		if s.Name == "HermesHTTP" && s.Section != elf.SHN_UNDEF {
			return true
		}
	}
	return false
}

func apiBase(d *Daemon) string {
	return "http://" + strings.TrimSuffix(strings.TrimPrefix(d.WSURL, "ws://"), "/") + "/api/v2"
}

type httpResult struct {
	status int
	header http.Header
	body   map[string]any
	raw    string
}

func call(t *testing.T, method, url, body string) httpResult {
	t.Helper()
	var rd io.Reader
	if body != "" {
		rd = strings.NewReader(body)
	}
	req, _ := http.NewRequest(method, url, rd)
	if body != "" {
		req.Header.Set("Content-Type", "application/json")
	}
	resp, err := (&http.Client{Timeout: 5 * time.Second}).Do(req)
	if err != nil {
		t.Fatalf("%s %s: %v", method, url, err)
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	r := httpResult{status: resp.StatusCode, header: resp.Header, raw: string(b)}
	_ = json.Unmarshal(b, &r.body)
	return r
}

func startAPIDaemon(t *testing.T, extra ...string) *Daemon {
	return startDaemon(t, daemonOpts{core: append([]string{"api_v2_enable = 1"}, extra...)})
}

// Off by default: /api/v2 is not served and its socket refused.
func TestAPIv2OffByDefault(t *testing.T) {
	d := startDaemon(t, daemonOpts{})
	if r := call(t, "GET", apiBase(d)+"/health", ""); r.status != 404 {
		t.Errorf("GET /api/v2/health with api_v2_enable off = %d, want 404", r.status)
	}
	if _, _, err := wsDialProto(strings.TrimSuffix(d.WSURL, "/")+"/api/v2/ws", "hermes.v1"); err == nil {
		t.Error("the hermes.v1 socket was accepted with api_v2_enable off")
	}
}

// Built without Go: /api/v2 says so with a problem, not a hang or a 404.
func TestAPIv2WithoutGo(t *testing.T) {
	if hasGoAPI(t) {
		t.Skip("this radio_daemon has the Go API layer")
	}
	d := startAPIDaemon(t)
	r := call(t, "GET", apiBase(d)+"/health", "")
	if r.status != 503 || r.header.Get("Content-Type") != "application/problem+json" || r.body["code"] != "not_supported" {
		t.Errorf("GET /api/v2/health without Go = %d %s", r.status, r.raw)
	}
}

func needGoAPI(t *testing.T) {
	t.Helper()
	if !hasGoAPI(t) {
		t.Skip("radio_daemon was built without the Go API layer (no go on the PATH)")
	}
}

// REST against the real radio: a profile PATCH moves the rig, as rigctl sees it.
func TestAPIv2REST(t *testing.T) {
	needGoAPI(t)
	d := startAPIDaemon(t)
	base := apiBase(d)
	rig := dialRigctl(t, d)

	r := call(t, "GET", base+"/health", "")
	if r.status != 200 || r.body["status"] != "ok" || r.header.Get("X-Request-Id") == "" {
		t.Fatalf("health = %d %s", r.status, r.raw)
	}
	r = call(t, "GET", base+"/radio", "")
	if r.status != 200 || r.body["frequency_hz"] != 7100000.0 || r.body["backend"] != "hamlib" {
		t.Errorf("radio = %d %s", r.status, r.raw)
	}

	r = call(t, "PATCH", base+"/radio/profiles/0", `{"frequency_hz":7074000,"mode":"USB","power":20}`)
	if r.status != 200 || r.body["frequency_hz"] != 7074000.0 {
		t.Fatalf("PATCH = %d %s", r.status, r.raw)
	}
	if got := rig.cmd("f"); got != "7074000" {
		t.Errorf("after the PATCH, rigctl f = %q, want 7074000", got)
	}
	r = call(t, "PATCH", base+"/radio/profiles/0", `{"power":300}`)
	if r.status != 422 || r.header.Get("Content-Type") != "application/problem+json" || r.body["code"] != "invalid" {
		t.Errorf("bad PATCH = %d %s", r.status, r.raw)
	}
	if r = call(t, "GET", base+"/radio/profiles/42", ""); r.status != 404 {
		t.Errorf("profile 42 = %d", r.status)
	}
	if r = call(t, "DELETE", base+"/radio", ""); r.status != 405 || r.header.Get("Allow") != "GET" {
		t.Errorf("DELETE /radio = %d, Allow %q", r.status, r.header.Get("Allow"))
	}
	if r = call(t, "GET", base+"/radio/digi", ""); r.status != 200 || r.body["voice_codec"] == nil {
		t.Errorf("digi settings = %d %s", r.status, r.raw)
	}
	if r = call(t, "GET", base+"/digi/log", ""); r.status != 200 || r.body["data"] == nil {
		t.Errorf("digi log = %d %s", r.status, r.raw)
	}
	if r = call(t, "GET", base+"/modem", ""); r.status != 503 || r.body["code"] != "not_running" {
		t.Errorf("modem without mercury = %d %s", r.status, r.raw)
	}
}

// Until logins exist, /api/v2 answers only on loopback.
func TestAPIv2LoopbackOnly(t *testing.T) {
	needGoAPI(t)
	var other string
	addrs, _ := net.InterfaceAddrs()
	for _, a := range addrs {
		if ip, ok := a.(*net.IPNet); ok && ip.IP.To4() != nil && !ip.IP.IsLoopback() {
			other = ip.IP.String()
			break
		}
	}
	if other == "" {
		t.Skip("no non-loopback IPv4 address on this machine")
	}
	d := startDaemon(t, daemonOpts{wsHost: "0.0.0.0", core: []string{"api_v2_enable = 1"}})
	port := d.WSURL[strings.LastIndex(d.WSURL, ":")+1 : len(d.WSURL)-1]
	r := call(t, "GET", "http://"+other+":"+port+"/api/v2/radio", "")
	if r.status != 403 || r.body["code"] != "forbidden" {
		t.Errorf("GET /api/v2/radio from %s = %d %s, want 403", other, r.status, r.raw)
	}
	if r = call(t, "GET", apiBase(d)+"/radio", ""); r.status != 200 {
		t.Errorf("from loopback = %d", r.status)
	}
}

// readFrame returns the next JSON text frame.
func readFrame(t *testing.T, ws *wsConn, timeout time.Duration) map[string]any {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for {
		op, msg, err := ws.readMessage(deadline)
		if err != nil {
			t.Fatalf("waiting for a hermes.v1 frame: %v", err)
		}
		if op != opText {
			continue
		}
		var m map[string]any
		if json.Unmarshal(msg, &m) == nil {
			return m
		}
	}
}

// hermes.v1: refused without the subprotocol; hello, a command that moves
// the rig, and the heartbeat with it. The legacy socket is unaffected.
func TestHermesV1(t *testing.T) {
	needGoAPI(t)
	d := startAPIDaemon(t)
	url := strings.TrimSuffix(d.WSURL, "/") + "/api/v2/ws"

	_, _, err := wsDialProto(url, "")
	var se *httpStatusError
	if !errors.As(err, &se) || se.status != 426 {
		t.Fatalf("upgrade without hermes.v1: %v, want HTTP 426", err)
	}

	ws, proto, err := wsDialProto(url, "hermes.v1")
	if err != nil || proto != "hermes.v1" {
		t.Fatalf("upgrade with hermes.v1: %v (subprotocol %q)\n--- daemon log ---\n%s", err, proto, d.Log())
	}
	defer ws.Close()
	hello := readFrame(t, ws, 5*time.Second)
	if hello["type"] != "hello" || hello["protocol"] != "hermes.v1" || hello["role"] != "admin" {
		t.Fatalf("hello = %v", hello)
	}
	if !strings.Contains(fmt.Sprint(hello["commands"]), "radio.set") {
		t.Errorf("hello commands = %v", hello["commands"])
	}

	if err := ws.writeFrame(opText, []byte(`{"type":"cmd","id":"7","cmd":"radio.set","args":{"frequency_hz":7080000}}`)); err != nil {
		t.Fatal(err)
	}
	var rep map[string]any
	for rep == nil || rep["type"] != "reply" {
		rep = readFrame(t, ws, 5*time.Second)
	}
	if rep["id"] != "7" || rep["ok"] != true {
		t.Fatalf("reply = %v", rep)
	}
	if got := dialRigctl(t, d).cmd("f"); got != "7080000" {
		t.Errorf("after radio.set, rigctl f = %q", got)
	}

	// The heartbeat (every 5 s), and nothing of the legacy protocol.
	deadline := time.Now().Add(7 * time.Second)
	for {
		f := readFrame(t, ws, time.Until(deadline))
		if f["type"] == "heartbeat" && f["ts"] != nil {
			break
		}
		if f["type"] == "state" {
			t.Fatalf("a legacy state frame reached the hermes.v1 socket: %v", f)
		}
	}

	// radiod's legacy socket still speaks its own protocol beside it.
	legacy := dialRadiod(t, d)
	if got := legacy.call("get_frequency", nil).Number(t); got != 7080000 {
		t.Errorf("legacy get_frequency = %d", got)
	}
}

// hermes.v1 topics through the real daemon: radio.state's snapshot, then an
// event when the frequency changes through radiod's rigctld port, a path
// that never touches the Go layer.
func TestHermesV1RadioStateTopic(t *testing.T) {
	needGoAPI(t)
	d := startAPIDaemon(t)
	ws, _, err := wsDialProto(strings.TrimSuffix(d.WSURL, "/")+"/api/v2/ws", "hermes.v1")
	if err != nil {
		t.Fatal(err)
	}
	defer ws.Close()
	if h := readFrame(t, ws, 5*time.Second); !strings.Contains(fmt.Sprint(h["topics"]), "radio.state") {
		t.Fatalf("hello topics = %v", h["topics"])
	}

	if err := ws.writeFrame(opText, []byte(`{"type":"subscribe","id":"s","topics":["radio.state"]}`)); err != nil {
		t.Fatal(err)
	}
	next := func(match func(map[string]any) bool) map[string]any {
		for {
			if f := readFrame(t, ws, 5*time.Second); match(f) {
				return f
			}
		}
	}
	if r := next(func(f map[string]any) bool { return f["type"] == "reply" }); r["ok"] != true {
		t.Fatalf("subscribe reply = %v", r)
	}
	isState := func(f map[string]any) bool { return f["type"] == "event" && f["topic"] == "radio.state" }
	snap := next(isState)
	if data, _ := snap["data"].(map[string]any); snap["snapshot"] != true || data["frequency_hz"] != 7100000.0 {
		t.Fatalf("snapshot = %v", snap)
	}

	if got := dialRigctl(t, d).cmd("F 7074000"); got != "RPRT 0" {
		t.Fatalf("rigctl F = %q", got)
	}
	ev := next(func(f map[string]any) bool {
		data, _ := f["data"].(map[string]any)
		return isState(f) && data["frequency_hz"] != nil
	})
	if data := ev["data"].(map[string]any); data["frequency_hz"] != 7074000.0 || ev["seq"] == 0.0 {
		t.Errorf("event after rigctl F = %v", ev)
	}
}

// /radio/settings through the radiod C API: read, change, read back. The
// dummy rig is a Hamlib radio, so it has no BFO.
func TestAPIv2RadioSettings(t *testing.T) {
	needGoAPI(t)
	d := startAPIDaemon(t)
	base := apiBase(d)
	r := call(t, "GET", base+"/radio/settings", "")
	if r.status != 200 || r.body["bfo_hz"] != nil || r.body["tx_timeout_s"] != nil || r.body["step_hz"] == nil {
		t.Fatalf("GET /radio/settings = %d %s", r.status, r.raw)
	}
	r = call(t, "PATCH", base+"/radio/settings", `{"step_hz":500,"swr_limit":3.2,"default_profile_fallback_s":120}`)
	if r.status != 200 || r.body["step_hz"] != 500.0 || r.body["swr_limit"] != 3.2 || r.body["default_profile_fallback_s"] != 120.0 {
		t.Fatalf("PATCH = %d %s", r.status, r.raw)
	}
	if r = call(t, "GET", base+"/radio/settings", ""); r.body["step_hz"] != 500.0 || r.body["swr_limit"] != 3.2 {
		t.Errorf("read back = %s", r.raw)
	}
	if r = call(t, "PATCH", base+"/radio/settings", `{"bfo_hz":40035000}`); r.status != 422 {
		t.Errorf("BFO on a Hamlib radio = %d %s", r.status, r.raw)
	}
}

// The Go layer logs through radiod's logger: its lines and radiod's share
// one stream, in hermes_log's format. With main:log_level = debug, a read is
// logged too.
func TestAPIv2LogsThroughRadiod(t *testing.T) {
	needGoAPI(t)
	d := startAPIDaemon(t, "log_level = debug")
	base := apiBase(d)
	call(t, "GET", base+"/radio", "")
	r := call(t, "PATCH", base+"/radio/profiles/0", `{"power":20}`)
	d.Stop()

	log := d.Log()
	for _, want := range []string{
		"[DBG] [api.http] GET /api/v2/radio 200 ms=",
		"[INF] [api.http] PATCH /api/v2/radio/profiles/0 200 ms=",
		"request_id=" + r.header.Get("X-Request-Id"),
		"[INF] [radiod] clean shutdown complete",
	} {
		if !strings.Contains(log, want) {
			t.Errorf("daemon log lacks %q\n--- daemon log ---\n%s", want, log)
		}
	}
}
