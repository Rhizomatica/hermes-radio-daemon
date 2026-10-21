// Mercury linked into radiod (main:enable_mercury): it starts with the
// daemon, serves its TNC ports, and stops with it.  Skipped when the
// binary was built without the vendor/mercury submodule.
//
// Copyright (C) 2026 Rhizomatica
// SPDX-License-Identifier: GPL-3.0-or-later

package integration

import (
	"bufio"
	"encoding/json"
	"fmt"
	"net"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// startMercuryDaemon starts radiod with mercury linked in, on ALSA's null
// device, its TNC on free ports and its chat store in the test's directory.
// Returns the daemon and the TNC control address.
func startMercuryDaemon(t *testing.T, core ...string) (*Daemon, string) {
	t.Helper()
	return startMercuryDaemonOpts(t, daemonOpts{core: core})
}

func startMercuryDaemonOpts(t *testing.T, o daemonOpts) (*Daemon, string) {
	t.Helper()
	if !hasMercury(t) {
		t.Skip("radio_daemon was built without mercury (vendor/mercury not checked out)")
	}
	ctl, bcast := freePortPair(t), freePort(t) // ctl+1 is the TNC data port
	for bcast == ctl || bcast == ctl+1 {
		bcast = freePort(t)
	}
	dir := t.TempDir()
	ini := filepath.Join(dir, "mercury.ini")
	writeFile(t, ini, fmt.Sprintf(`[main]
arq_tcp_base_port = %d
broadcast_tcp_port = %d
[ptt]
method = none
[store]
enabled = true
path = %s
`, ctl, bcast, filepath.Join(dir, "messages.jsonl")))

	o.core = append([]string{
		"enable_mercury = 1",
		"mercury_config = " + ini,
		"capture_device = null",
		"playback_device = null",
	}, o.core...)
	d := startDaemon(t, o)
	addr := fmt.Sprintf("127.0.0.1:%d", ctl)
	if err := d.waitPort(addr, 15*time.Second); err != nil {
		t.Fatalf("mercury's TNC: %v\n--- daemon log ---\n%s", err, d.Log())
	}
	return d, addr
}

// tncCommand sends a CR-terminated TNC command and returns the first reply
// line that is not an asynchronous report (IAMALIVE, BUFFER, BUSY, ...).
func tncCommand(t *testing.T, conn net.Conn, r *bufio.Reader, cmd string) string {
	t.Helper()
	_ = conn.SetDeadline(time.Now().Add(5 * time.Second))
	if cmd != "" { // "": only read the next line
		if _, err := fmt.Fprintf(conn, "%s\r", cmd); err != nil {
			t.Fatalf("TNC %q: %v", cmd, err)
		}
	}
	for {
		line, err := r.ReadString('\r')
		if err != nil {
			t.Fatalf("TNC %q: %v", cmd, err)
		}
		line = strings.TrimSpace(line)
		switch {
		case line == "", strings.HasPrefix(line, "IAMALIVE"), strings.HasPrefix(line, "BUFFER"),
			strings.HasPrefix(line, "BUSY"), strings.HasPrefix(line, "SN "):
			continue
		}
		return line
	}
}

func TestMercuryTNCAnswers(t *testing.T) {
	d, addr := startMercuryDaemon(t)
	conn, err := net.DialTimeout("tcp", addr, 5*time.Second)
	if err != nil {
		t.Fatalf("TNC control %s: %v", addr, err)
	}
	defer conn.Close()
	r := bufio.NewReader(conn)

	// As mercury's own integration test: MYCALL is answered OK, then
	// acknowledged per call with REGISTERED.
	steps := []struct{ cmd, want, then string }{
		{"MYCALL TESTA", "OK", "REGISTERED TESTA"},
		{"LISTEN ON", "OK", ""},
	}
	for _, s := range steps {
		if got := tncCommand(t, conn, r, s.cmd); got != s.want {
			t.Fatalf("TNC %q = %q, want %q\n--- daemon log ---\n%s", s.cmd, got, s.want, d.Log())
		}
		if s.then != "" {
			if got := tncCommand(t, conn, r, ""); got != s.then {
				t.Fatalf("after TNC %q: %q, want %q", s.cmd, got, s.then)
			}
		}
	}

	// radiod's own interfaces are still there beside it.
	ws := dialRadiod(t, d)
	if got := ws.call("get_txrx_status", nil).String(t); got != "INRX" {
		t.Errorf("get_txrx_status = %q with mercury idle, want INRX", got)
	}

	conn.Close()
	d.Stop()
	// radiod's lines are hermes_log's, as mercury's are: "[INF] [component] message".
	for _, want := range []string{"[INF] [mercury-link] mercury starting", "[INF] [mercury-link] mercury stopped"} {
		if !strings.Contains(d.Log(), want) {
			t.Errorf("daemon log lacks %q\n--- daemon log ---\n%s", want, d.Log())
		}
	}
}

// waitJSON reads messages until a JSON text frame satisfies match, and
// counts the binary frames passed on the way by their first (type) byte.
func waitJSON(t *testing.T, ws *wsConn, timeout time.Duration, binTypes map[byte]int,
	match func(map[string]any) bool) map[string]any {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for {
		op, msg, err := ws.readMessage(deadline)
		if err != nil {
			t.Fatalf("waiting for a matching JSON frame: %v", err)
		}
		if op == opBinary {
			if len(msg) > 0 && binTypes != nil {
				binTypes[msg[0]]++
			}
			continue
		}
		var j map[string]any
		if json.Unmarshal(msg, &j) == nil && match(j) {
			return j
		}
	}
}

// Everything mercury's own websocket would carry reaches radiod's: hello
// lists it, its status (with the TNC's callsign) and history arrive, and a
// mercury command is carried out and answered on the same connection.
func TestMercuryUIOnRadiodWebsocket(t *testing.T) {
	d, addr := startMercuryDaemon(t)

	ws, err := wsDial(d.WSURL)
	if err != nil {
		t.Fatal(err)
	}
	defer ws.Close()

	hello := waitJSON(t, ws, 5*time.Second, nil, func(j map[string]any) bool { return j["type"] == "hello" })
	mods, _ := hello["modules"].([]any)
	if len(mods) != 1 || mods[0] != "mercury" {
		t.Errorf("hello modules = %v, want [mercury]", hello["modules"])
	}
	if got := hello["mercury_binary_type"]; got != float64(4) {
		t.Errorf("hello mercury_binary_type = %v, want 4", got)
	}

	// The one-time state a new client gets, then the 500 ms status.
	waitJSON(t, ws, 5*time.Second, nil, func(j map[string]any) bool { return j["type"] == "history" })
	waitJSON(t, ws, 5*time.Second, nil, func(j map[string]any) bool { return j["type"] == "status" })

	// A callsign set on the TNC shows in the status on the websocket.
	conn, err := net.DialTimeout("tcp", addr, 5*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()
	r := bufio.NewReader(conn)
	if got := tncCommand(t, conn, r, "MYCALL TESTC"); got != "OK" {
		t.Fatalf("TNC MYCALL = %q", got)
	}
	waitJSON(t, ws, 5*time.Second, nil, func(j map[string]any) bool {
		return j["type"] == "status" && j["user_callsign"] == "TESTC"
	})

	// A mercury command over radiod's websocket: answered, and carried out.
	if err := ws.writeFrame(opText, []byte(`{"command":"set_tx_gain","value":"6"}`)); err != nil {
		t.Fatal(err)
	}
	ack := waitJSON(t, ws, 5*time.Second, nil, func(j map[string]any) bool { return j["command"] == "set_tx_gain" })
	if ack["status"] != "ok" {
		t.Fatalf("set_tx_gain reply = %v, want status ok", ack)
	}
	bins := map[byte]int{}
	waitJSON(t, ws, 5*time.Second, bins, func(j map[string]any) bool {
		g, ok := j["tx_gain_db"].(float64)
		return j["type"] == "status" && ok && g > 5.9 && g < 6.1
	})

	// A command mercury refuses is answered as refused.
	if err := ws.writeFrame(opText, []byte(`{"command":"no_such_command"}`)); err != nil {
		t.Fatal(err)
	}
	nack := waitJSON(t, ws, 5*time.Second, nil, func(j map[string]any) bool { return j["command"] == "no_such_command" })
	if nack["status"] != "error" {
		t.Errorf("no_such_command reply = %v, want status error", nack)
	}

	// radiod's own commands still work beside mercury's.
	c := &radiodClient{t: t, ws: ws}
	if got := c.call("get_txrx_status", nil).String(t); got != "INRX" {
		t.Errorf("get_txrx_status = %q", got)
	}

	// Mercury's spectrum: type 4, then its own frame ("YRCM", FFT size,
	// sample rate, float32 bins), unchanged.
	deadline := time.Now().Add(5 * time.Second)
	for {
		op, msg, err := ws.readMessage(deadline)
		if err != nil {
			t.Fatalf("no mercury spectrum frame: %v", err)
		}
		if op != opBinary || len(msg) == 0 || msg[0] != 4 {
			continue
		}
		if len(msg) < 9 || string(msg[1:5]) != "YRCM" {
			t.Fatalf("mercury frame starts % x, want 04 then \"YRCM\"", msg[:min(len(msg), 9)])
		}
		bins := int(msg[5]) | int(msg[6])<<8
		if want := 1 + 8 + 4*bins; len(msg) != want {
			t.Errorf("mercury frame of %d bytes for %d bins, want %d", len(msg), bins, want)
		}
		break
	}

	// Binary frames on the way were only radiod's (1-3) or mercury's (4).
	for b := range bins {
		if b < 1 || b > 4 {
			t.Errorf("binary frame of unknown type %d", b)
		}
	}
	t.Logf("binary frames seen by type: %v", bins)
}
