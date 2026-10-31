// The /api/v2 REST API and the hermes.v1 websocket (api, Go,
// linked into radiod as a C archive) through the real daemon.
//
// Copyright (C) 2026 Rhizomatica
// SPDX-License-Identifier: GPL-3.0-or-later

package integration

import (
	"bufio"
	"debug/elf"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"regexp"
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
	return callAuth(t, method, url, body, "")
}

// callAuth is call with a session token (Authorization: Bearer).
func callAuth(t *testing.T, method, url, body, token string) httpResult {
	t.Helper()
	var rd io.Reader
	if body != "" {
		rd = strings.NewReader(body)
	}
	req, _ := http.NewRequest(method, url, rd)
	if body != "" {
		req.Header.Set("Content-Type", "application/json")
	}
	if token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
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

// setupAPI runs first-boot setup on a freshly started daemon and returns the
// new admin's session token. startDaemon points the daemon at a temp database
// and a known setup code (SETUP_CODE_PATH), so this is deterministic.
func setupAPI(t *testing.T, d *Daemon) string {
	t.Helper()
	r := call(t, "POST", apiBase(d)+"/setup",
		`{"setup_code":"`+apiSetupCode+`","station_name":"Test Station","callsign":"TEST1",`+
			`"country":"BR","language":"en","timezone":"America/Sao_Paulo",`+
			`"admin":{"username":"admin","password":"password1","display_name":"Admin"}}`)
	if r.status != 201 {
		t.Fatalf("POST /setup = %d %s\n--- daemon log ---\n%s", r.status, r.raw, d.Log())
	}
	token, _ := r.body["token"].(string)
	if token == "" {
		t.Fatalf("POST /setup returned no token: %s", r.raw)
	}
	return token
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
	token := setupAPI(t, d)
	base := apiBase(d)
	rig := dialRigctl(t, d)

	r := call(t, "GET", base+"/health", "")
	if r.status != 200 || r.body["status"] != "ok" || r.header.Get("X-Request-Id") == "" {
		t.Fatalf("health = %d %s", r.status, r.raw)
	}
	r = callAuth(t, "GET", base+"/radio", "", token)
	if r.status != 200 || r.body["frequency_hz"] != 7100000.0 || r.body["backend"] != "hamlib" {
		t.Errorf("radio = %d %s", r.status, r.raw)
	}

	r = callAuth(t, "PATCH", base+"/radio/profiles/0", `{"frequency_hz":7074000,"mode":"USB","power":20}`, token)
	if r.status != 200 || r.body["frequency_hz"] != 7074000.0 {
		t.Fatalf("PATCH = %d %s", r.status, r.raw)
	}
	if got := rig.cmd("f"); got != "7074000" {
		t.Errorf("after the PATCH, rigctl f = %q, want 7074000", got)
	}
	r = callAuth(t, "PATCH", base+"/radio/profiles/0", `{"power":300}`, token)
	if r.status != 422 || r.header.Get("Content-Type") != "application/problem+json" || r.body["code"] != "invalid" {
		t.Errorf("bad PATCH = %d %s", r.status, r.raw)
	}
	if r = callAuth(t, "GET", base+"/radio/profiles/42", "", token); r.status != 404 {
		t.Errorf("profile 42 = %d", r.status)
	}
	if r = callAuth(t, "DELETE", base+"/radio", "", token); r.status != 405 || r.header.Get("Allow") != "GET" {
		t.Errorf("DELETE /radio = %d, Allow %q", r.status, r.header.Get("Allow"))
	}
	if r = callAuth(t, "GET", base+"/radio/digi", "", token); r.status != 200 || r.body["voice_codec"] == nil {
		t.Errorf("digi settings = %d %s", r.status, r.raw)
	}
	if r = callAuth(t, "GET", base+"/digi/log", "", token); r.status != 200 || r.body["data"] == nil {
		t.Errorf("digi log = %d %s", r.status, r.raw)
	}
	if r = callAuth(t, "GET", base+"/modem", "", token); r.status != 503 || r.body["code"] != "not_running" {
		t.Errorf("modem without mercury = %d %s", r.status, r.raw)
	}
}

// Remote clients are answered now that sessions exist: without a session
// they get 401, and with a valid session they get through — REST and
// hermes.v1 alike.
func TestAPIv2RemoteAuth(t *testing.T) {
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
	token := setupAPI(t, d)
	port := d.WSURL[strings.LastIndex(d.WSURL, ":")+1 : len(d.WSURL)-1]
	base := "http://" + other + ":" + port + "/api/v2"

	if r := call(t, "GET", base+"/radio", ""); r.status != 401 || r.body["code"] != "unauthenticated" {
		t.Errorf("GET /api/v2/radio from %s without a session = %d %s, want 401 unauthenticated", other, r.status, r.raw)
	}
	if r := callAuth(t, "GET", base+"/radio", "", token); r.status != 200 {
		t.Errorf("GET /api/v2/radio from %s with a session = %d %s, want 200", other, r.status, r.raw)
	}

	wsURL := "ws://" + other + ":" + port + "/api/v2/ws"
	_, _, err := wsDialProto(wsURL, "hermes.v1")
	var se *httpStatusError
	if !errors.As(err, &se) || se.status != 401 {
		t.Errorf("hermes.v1 remote upgrade with no session = %v, want HTTP 401", err)
	}
	ws, proto, err := wsDialAuth(wsURL, "hermes.v1", token)
	if err != nil {
		t.Fatalf("hermes.v1 remote upgrade with a session: %v", err)
	}
	defer ws.Close()
	if proto != "hermes.v1" {
		t.Errorf("hermes.v1 remote protocol = %q, want hermes.v1", proto)
	}
	if f := readFrame(t, ws, time.Second); f["type"] != "hello" {
		t.Errorf("hermes.v1 remote hello = %v", f)
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
	token := setupAPI(t, d)
	url := strings.TrimSuffix(d.WSURL, "/") + "/api/v2/ws"

	_, _, err := wsDialProto(url, "")
	var se *httpStatusError
	if !errors.As(err, &se) || se.status != 426 {
		t.Fatalf("upgrade without hermes.v1: %v, want HTTP 426", err)
	}

	ws, proto, err := wsDialAuth(url, "hermes.v1", token)
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
	token := setupAPI(t, d)
	ws, _, err := wsDialAuth(strings.TrimSuffix(d.WSURL, "/")+"/api/v2/ws", "hermes.v1", token)
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
	// the snapshot, then the reply
	isState := func(f map[string]any) bool { return f["type"] == "event" && f["topic"] == "radio.state" }
	if snap := next(isState); snap["snapshot"] != true ||
		snap["data"].(map[string]any)["frequency_hz"] != 7100000.0 {
		t.Fatalf("snapshot = %v", snap)
	}
	if r := next(func(f map[string]any) bool { return f["type"] == "reply" }); r["ok"] != true {
		t.Fatalf("subscribe reply = %v", r)
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
	token := setupAPI(t, d)
	base := apiBase(d)
	r := callAuth(t, "GET", base+"/radio/settings", "", token)
	if r.status != 200 || r.body["bfo_hz"] != nil || r.body["tx_timeout_s"] != nil || r.body["step_hz"] == nil {
		t.Fatalf("GET /radio/settings = %d %s", r.status, r.raw)
	}
	r = callAuth(t, "PATCH", base+"/radio/settings", `{"step_hz":500,"swr_limit":3.2,"default_profile_fallback_s":120}`, token)
	if r.status != 200 || r.body["step_hz"] != 500.0 || r.body["swr_limit"] != 3.2 || r.body["default_profile_fallback_s"] != 120.0 {
		t.Fatalf("PATCH = %d %s", r.status, r.raw)
	}
	if r = callAuth(t, "GET", base+"/radio/settings", "", token); r.body["step_hz"] != 500.0 || r.body["swr_limit"] != 3.2 {
		t.Errorf("read back = %s", r.raw)
	}
	if r = callAuth(t, "PATCH", base+"/radio/settings", `{"bfo_hz":40035000}`, token); r.status != 422 {
		t.Errorf("BFO on a Hamlib radio = %d %s", r.status, r.raw)
	}
}

// The Go layer logs through radiod's logger: its lines and radiod's share
// one stream, in hermes_log's format. With main:log_level = debug, a read is
// logged too.
func TestAPIv2LogsThroughRadiod(t *testing.T) {
	needGoAPI(t)
	d := startAPIDaemon(t, "log_level = debug")
	token := setupAPI(t, d)
	base := apiBase(d)
	callAuth(t, "GET", base+"/radio", "", token)
	r := callAuth(t, "PATCH", base+"/radio/profiles/0", `{"power":20}`, token)
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

// modem.status through the real daemon with mercury inside: subscribe once
// mercury has published a status, then change the callsign on mercury's TNC
// port and see it in an event.
func TestHermesV1ModemStatusTopic(t *testing.T) {
	needGoAPI(t)
	d, tnc := startMercuryDaemon(t, "api_v2_enable = 1")
	token := setupAPI(t, d)
	ws, _, err := wsDialAuth(strings.TrimSuffix(d.WSURL, "/")+"/api/v2/ws", "hermes.v1", token)
	if err != nil {
		t.Fatal(err)
	}
	defer ws.Close()
	next := func(match func(map[string]any) bool) map[string]any {
		for {
			if f := readFrame(t, ws, 10*time.Second); match(f) {
				return f
			}
		}
	}
	isStatus := func(f map[string]any) bool { return f["type"] == "event" && f["topic"] == "modem.status" }

	// mercury publishes its first status within about a second of starting
	var snap map[string]any
	for i := 0; snap == nil; i++ {
		id := fmt.Sprintf("s%d", i)
		_ = ws.writeFrame(opText, []byte(`{"type":"subscribe","id":"`+id+`","topics":["modem.status"]}`))
		// the snapshot comes before the reply
		var seen map[string]any
		r := next(func(f map[string]any) bool {
			if isStatus(f) && seen == nil {
				seen = f
			}
			return f["type"] == "reply" && f["id"] == id
		})
		if r["ok"] == true {
			if snap = seen; snap == nil {
				t.Fatalf("subscribe replied with no snapshot before it")
			}
			break
		}
		if i == 20 {
			t.Fatalf("modem.status never became available: %v\n--- daemon log ---\n%s", r, d.Log())
		}
		time.Sleep(500 * time.Millisecond)
	}
	if data, _ := snap["data"].(map[string]any); snap["snapshot"] != true || data["link"] == nil || data["audio_ok"] == nil {
		t.Fatalf("snapshot = %v", snap)
	}

	conn, err := net.DialTimeout("tcp", tnc, 5*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	if got := tncCommand(t, conn, bufio.NewReader(conn), "MYCALL N0CALL"); got != "OK" {
		t.Fatalf("TNC MYCALL = %q", got)
	}
	ev := next(func(f map[string]any) bool {
		data, _ := f["data"].(map[string]any)
		return isStatus(f) && data["my_call"] != nil
	})
	if data := ev["data"].(map[string]any); data["my_call"] != "N0CALL" || ev["seq"] == 0.0 {
		t.Errorf("event after MYCALL = %v", ev)
	}
}

// The radio's controls through the real daemon (the harness's dummy rig): rig.set
// reaches the rig as rigctl sees it, rig.get reads it back, and rig.watch
// gives radio.rig events.
func TestHermesV1RigControls(t *testing.T) {
	needGoAPI(t)
	d := startAPIDaemon(t)
	token := setupAPI(t, d)
	ws, _, err := wsDialAuth(strings.TrimSuffix(d.WSURL, "/")+"/api/v2/ws", "hermes.v1", token)
	if err != nil {
		t.Fatal(err)
	}
	defer ws.Close()
	if h := readFrame(t, ws, 5*time.Second); !strings.Contains(fmt.Sprint(h["topics"]), "radio.rig") {
		t.Fatalf("hello topics = %v", h["topics"])
	}
	cmd := func(id, frame string) map[string]any {
		if err := ws.writeFrame(opText, []byte(frame)); err != nil {
			t.Fatal(err)
		}
		for {
			if f := readFrame(t, ws, 5*time.Second); f["type"] == "reply" && f["id"] == id {
				return f
			}
		}
	}
	if r := cmd("s", `{"type":"cmd","id":"s","cmd":"rig.set","args":{"name":"AF","value":0.25}}`); r["ok"] != true {
		t.Fatalf("rig.set AF = %v", r)
	}
	if got := dialRigctl(t, d).cmd("l AF"); !strings.HasPrefix(got, "0.25") {
		t.Errorf("rigctl l AF after rig.set = %q", got)
	}
	r := cmd("g", `{"type":"cmd","id":"g","cmd":"rig.get","args":{"names":["AF"]}}`)
	if lv, _ := r["data"].(map[string]any)["levels"].(map[string]any); r["ok"] != true || lv["AF"] != 0.25 {
		t.Fatalf("rig.get AF = %v", r)
	}

	cmd("sub", `{"type":"subscribe","id":"sub","topics":["radio.rig"]}`)
	cmd("w", `{"type":"cmd","id":"w","cmd":"rig.watch","args":{"names":["AF"],"interval_ms":250}}`)
	if got := dialRigctl(t, d).cmd("L AF 0.75"); got != "RPRT 0" {
		t.Fatalf("rigctl L AF = %q", got)
	}
	for deadline := time.Now().Add(5 * time.Second); ; {
		f := readFrame(t, ws, time.Until(deadline))
		if f["type"] != "event" || f["topic"] != "radio.rig" {
			continue
		}
		if lv, _ := f["data"].(map[string]any)["levels"].(map[string]any); lv["AF"] == 0.75 {
			break
		}
	}
}

// waitModem polls GET path until mercury inside the daemon has published a
// status (503 until then).
func waitModem(t *testing.T, d *Daemon, token, path string) httpResult {
	t.Helper()
	for i := 0; ; i++ {
		r := callAuth(t, "GET", apiBase(d)+path, "", token)
		if r.status == 200 {
			return r
		}
		if i == 40 {
			t.Fatalf("GET %s = %d %s\n--- daemon log ---\n%s", path, r.status, r.raw, d.Log())
		}
		time.Sleep(250 * time.Millisecond)
	}
}

// The listen mode through the real daemon: PATCH /modem/config sets it in
// mercury and saves it in user.ini, and a daemon started with it saved
// sets it again.
func TestAPIv2ModemListenMode(t *testing.T) {
	needGoAPI(t)
	d, _ := startMercuryDaemon(t, "api_v2_enable = 1")
	token := setupAPI(t, d)
	if c := waitModem(t, d, token, "/modem/config"); c.body["listen_mode"] == nil {
		t.Errorf("GET /modem/config = %s", c.raw)
	}
	if r := callAuth(t, "PATCH", apiBase(d)+"/modem/config", `{"listen_mode":"DATAC0"}`, token); r.status != 422 {
		t.Errorf("PATCH DATAC0 = %d %s", r.status, r.raw)
	}
	r := callAuth(t, "PATCH", apiBase(d)+"/modem/config", `{"listen_mode":"DATAC1"}`, token)
	if r.status != 200 || r.body["listen_mode"] != "DATAC1" {
		t.Fatalf("PATCH DATAC1 = %d %s\n--- daemon log ---\n%s", r.status, r.raw, d.Log())
	}
	// mercury's status says it within a second or so
	for i := 0; ; i++ {
		if s := callAuth(t, "GET", apiBase(d)+"/modem", "", token); s.body["listen_mode"] == "DATAC1" {
			break
		} else if i == 20 {
			t.Fatalf("GET /modem listen_mode = %v after the PATCH", s.body["listen_mode"])
		}
		time.Sleep(250 * time.Millisecond)
	}
	d.Stop() // writes user.ini
	if b, _ := os.ReadFile(d.UserINI); !regexp.MustCompile(`(?m)^mercury_listen_mode\s*=\s*"?DATAC1"?$`).Match(b) {
		t.Errorf("user.ini lacks mercury_listen_mode = DATAC1:\n%s", b)
	}

	d2, _ := startMercuryDaemonOpts(t, daemonOpts{core: []string{"api_v2_enable = 1"},
		userMain: []string{"mercury_listen_mode = DATAC4"}})
	token2 := setupAPI(t, d2)
	if c := waitModem(t, d2, token2, "/modem/config"); c.body["listen_mode"] != "DATAC4" {
		t.Errorf("listen mode from user.ini = %v\n--- daemon log ---\n%s", c.body["listen_mode"], d2.Log())
	}
	if !strings.Contains(d2.Log(), "[mercury-link] listen mode DATAC4") {
		t.Errorf("daemon log lacks the listen mode\n%s", d2.Log())
	}
}

// Mercury's chat through the real daemon: a broadcast line sent with
// modem.chat.send comes back on modem.chat as sent; an ARQ line with no
// session is refused.
func TestHermesV1ModemChat(t *testing.T) {
	needGoAPI(t)
	d, _ := startMercuryDaemon(t, "api_v2_enable = 1")
	token := setupAPI(t, d)
	waitModem(t, d, token, "/modem")
	ws, _, err := wsDialAuth(strings.TrimSuffix(d.WSURL, "/")+"/api/v2/ws", "hermes.v1", token)
	if err != nil {
		t.Fatal(err)
	}
	defer ws.Close()
	next := func(match func(map[string]any) bool) map[string]any {
		// the heartbeat keeps frames coming: give up on the match itself
		for end := time.Now().Add(10 * time.Second); time.Now().Before(end); {
			if f := readFrame(t, ws, 10*time.Second); match(f) {
				return f
			}
		}
		t.Fatalf("no such frame in 10 s\n--- daemon log ---\n%s", d.Log())
		return nil
	}
	reply := func(id string) func(map[string]any) bool {
		return func(f map[string]any) bool { return f["type"] == "reply" && f["id"] == id }
	}
	_ = ws.writeFrame(opText, []byte(`{"type":"subscribe","id":"s","topics":["modem.chat"]}`))
	if r := next(reply("s")); r["ok"] != true {
		t.Fatalf("subscribe = %v", r)
	}
	_ = ws.writeFrame(opText, []byte(`{"type":"cmd","id":"c1","cmd":"modem.chat.send","args":{"plane":"broadcast","text":"CQ test"}}`))
	if r := next(reply("c1")); r["ok"] != true {
		t.Fatalf("send = %v\n--- daemon log ---\n%s", r, d.Log())
	}
	ev := next(func(f map[string]any) bool { return f["type"] == "event" && f["topic"] == "modem.chat" })
	if data := ev["data"].(map[string]any); data["text"] != "CQ test" || data["dir"] != "tx" || data["plane"] != "broadcast" {
		t.Errorf("chat event = %v", ev)
	}
	_ = ws.writeFrame(opText, []byte(`{"type":"cmd","id":"c2","cmd":"modem.chat.send","args":{"plane":"arq","text":"x"}}`))
	if r := next(reply("c2")); r["ok"] != false || r["error"].(map[string]any)["code"] != "no_link" {
		t.Errorf("ARQ send with no session = %v, want no_link", r)
	}
}

// v1cmd sends one hermes.v1 command and returns its reply.
func v1cmd(t *testing.T, ws *wsConn, id, frame string) map[string]any {
	t.Helper()
	if err := ws.writeFrame(opText, []byte(frame)); err != nil {
		t.Fatal(err)
	}
	for {
		if r := readFrame(t, ws, 5*time.Second); r["type"] == "reply" && r["id"] == id {
			return r
		}
	}
}

// radio.ptt through the real daemon: the connection that keys owns PTT,
// another is refused, and closing the owner's socket unkeys the radio (C's
// close path, which no unit test reaches).
func TestHermesV1PTT(t *testing.T) {
	needGoAPI(t)
	d := startAPIDaemon(t)
	token := setupAPI(t, d)
	url := strings.TrimSuffix(d.WSURL, "/") + "/api/v2/ws"
	rig := dialRigctl(t, d)

	a, _, err := wsDialAuth(url, "hermes.v1", token)
	if err != nil {
		t.Fatal(err)
	}
	b, _, err := wsDialAuth(url, "hermes.v1", token)
	if err != nil {
		t.Fatal(err)
	}
	defer b.Close()

	if r := v1cmd(t, a, "1", `{"type":"cmd","id":"1","cmd":"radio.ptt","args":{"on":true}}`); r["ok"] != true {
		t.Fatalf("key = %v\n--- daemon log ---\n%s", r, d.Log())
	}
	if got := rig.cmd("t"); got != "1" {
		t.Fatalf("after radio.ptt on, rigctl t = %q", got)
	}
	r := v1cmd(t, b, "2", `{"type":"cmd","id":"2","cmd":"radio.ptt","args":{"on":true}}`)
	if e, _ := r["error"].(map[string]any); r["ok"] != false || e["code"] != "busy" {
		t.Fatalf("a second connection keying = %v, want busy", r)
	}
	r = v1cmd(t, b, "3", `{"type":"cmd","id":"3","cmd":"radio.ptt","args":{"on":false}}`)
	if e, _ := r["error"].(map[string]any); e["code"] != "no_ptt" || rig.cmd("t") != "1" {
		t.Fatalf("a second connection unkeying = %v", r)
	}

	a.Close()
	if !eventually(5*time.Second, func() bool { return rig.cmd("t") == "0" }) {
		t.Fatalf("closing the keying socket left the radio in TX\n--- daemon log ---\n%s", d.Log())
	}
	if r := v1cmd(t, b, "4", `{"type":"cmd","id":"4","cmd":"radio.ptt","args":{"on":true}}`); r["ok"] != true {
		t.Fatalf("keying after the holder left = %v", r)
	}
	if r := v1cmd(t, b, "5", `{"type":"cmd","id":"5","cmd":"radio.ptt","args":{"on":false}}`); r["ok"] != true || rig.cmd("t") != "0" {
		t.Fatalf("unkey = %v", r)
	}
}
