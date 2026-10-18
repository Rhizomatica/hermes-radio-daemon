// radiod over its websocket and rigctl interfaces, on Hamlib's dummy rig.
//
// Copyright (C) 2026 Rhizomatica
// SPDX-License-Identifier: GPL-3.0-or-later

package integration

import (
	"os"
	"regexp"
	"strconv"
	"testing"
	"time"
)

// The daemon starts, serves both interfaces, and shuts down cleanly on
// SIGTERM (Stop checks the exit status and the log).
func TestStartAndStop(t *testing.T) {
	d := startDaemon(t, daemonOpts{freq: 7100000})
	ws := dialRadiod(t, d)
	if got := ws.call("get_frequency", nil).Number(t); got != 7100000 {
		t.Fatalf("get_frequency = %d, want 7100000 (user.ini's profile 0)", got)
	}
	rig := dialRigctl(t, d)
	if got := rig.cmd("f"); got != "7100000" {
		t.Fatalf("rigctl f = %q, want 7100000", got)
	}
	d.Stop()
}

// A frequency or mode set on one interface reads back the same on the other.
func TestFrequencyAndModeAcrossInterfaces(t *testing.T) {
	d := startDaemon(t, daemonOpts{})
	ws := dialRadiod(t, d)
	rig := dialRigctl(t, d)

	if r := ws.call("set_frequency", map[string]any{"frequency": 7105000}); !r.OK {
		t.Fatalf("set_frequency: %+v", r)
	}
	if got := rig.cmd("f"); got != "7105000" {
		t.Errorf("after websocket set_frequency, rigctl f = %q, want 7105000", got)
	}

	if got := rig.cmd("F 7110000"); got != "RPRT 0" {
		t.Fatalf("rigctl F 7110000 = %q", got)
	}
	if got := ws.call("get_frequency", nil).Number(t); got != 7110000 {
		t.Errorf("after rigctl F, get_frequency = %d, want 7110000", got)
	}

	if r := ws.call("set_mode", map[string]any{"mode": "LSB"}); !r.OK {
		t.Fatalf("set_mode LSB: %+v", r)
	}
	if got := rig.cmdLines("m", 2)[0]; got != "LSB" {
		t.Errorf("after websocket set_mode LSB, rigctl m = %q", got)
	}
	if got := ws.call("get_mode", nil).String(t); got != "LSB" {
		t.Errorf("get_mode = %q, want LSB", got)
	}
	if r := ws.call("set_mode", map[string]any{"mode": "NOSUCHMODE"}); r.OK {
		t.Errorf("set_mode NOSUCHMODE was accepted: %+v", r)
	}
}

// The profile's frequency is written back to user.ini, so it survives a
// restart.
func TestFrequencyPersistsInUserINI(t *testing.T) {
	d := startDaemon(t, daemonOpts{freq: 7100000})
	ws := dialRadiod(t, d)
	if r := ws.call("set_frequency", map[string]any{"frequency": 7123000}); !r.OK {
		t.Fatalf("set_frequency: %+v", r)
	}
	d.Stop()

	b, err := os.ReadFile(d.UserINI)
	if err != nil {
		t.Fatal(err)
	}
	m := regexp.MustCompile(`(?ms)^\[profile0\].*?^freq\s*=\s*"?(\d+)"?`).FindSubmatch(b)
	if m == nil {
		t.Fatalf("no profile0 freq in user.ini:\n%s", b)
	}
	if got, _ := strconv.Atoi(string(m[1])); got != 7123000 {
		t.Errorf("user.ini profile0 freq = %d, want 7123000", got)
	}
}

// PTT from the websocket keys the radio; both interfaces see it; a second
// ptt_on while keyed is refused; ptt_off unkeys.
func TestPTTFromWebsocket(t *testing.T) {
	d := startDaemon(t, daemonOpts{})
	ws := dialRadiod(t, d)
	rig := dialRigctl(t, d)

	if r := ws.call("ptt_on", nil); !r.OK {
		t.Fatalf("ptt_on: %+v", r)
	}
	if got := ws.call("get_txrx_status", nil).String(t); got != "INTX" {
		t.Errorf("get_txrx_status = %q, want INTX", got)
	}
	if got := rig.cmd("t"); got != "1" {
		t.Errorf("rigctl t = %q while keyed, want 1", got)
	}
	if r := ws.call("ptt_on", nil); r.OK || r.Status != "NOK" {
		t.Errorf("a second ptt_on while keyed: %+v, want refused with NOK", r)
	}

	if r := ws.call("ptt_off", nil); !r.OK {
		t.Fatalf("ptt_off: %+v", r)
	}
	if got := ws.call("get_txrx_status", nil).String(t); got != "INRX" {
		t.Errorf("get_txrx_status = %q after ptt_off, want INRX", got)
	}
	if got := rig.cmd("t"); got != "0" {
		t.Errorf("rigctl t = %q after ptt_off, want 0", got)
	}
}

// A websocket client that keys and then disconnects does not leave the
// radio transmitting.
func TestPTTReleasedWhenWebsocketKeyerDisconnects(t *testing.T) {
	d := startDaemon(t, daemonOpts{})
	keyer := dialRadiod(t, d)
	watcher := dialRadiod(t, d)

	if r := keyer.call("ptt_on", nil); !r.OK {
		t.Fatalf("ptt_on: %+v", r)
	}
	keyer.ws.conn.Close() // gone without a close frame, as a crashed client is

	if !eventually(3*time.Second, func() bool {
		return watcher.call("get_txrx_status", nil).String(t) == "INRX"
	}) {
		t.Fatalf("still transmitting 3 s after the keyer disconnected\n--- daemon log ---\n%s", d.Log())
	}
}

// The same for a rigctl client (hamlib's rigctld keying, e.g. from a
// digital-mode program that crashed).
func TestPTTReleasedWhenRigctlKeyerDisconnects(t *testing.T) {
	d := startDaemon(t, daemonOpts{})
	keyer := dialRigctl(t, d)
	watcher := dialRadiod(t, d)

	if got := keyer.cmd("T 1"); got != "RPRT 0" {
		t.Fatalf("rigctl T 1 = %q", got)
	}
	if got := watcher.call("get_txrx_status", nil).String(t); got != "INTX" {
		t.Fatalf("get_txrx_status = %q after rigctl T 1, want INTX", got)
	}
	keyer.conn.Close()

	if !eventually(3*time.Second, func() bool {
		return watcher.call("get_txrx_status", nil).String(t) == "INRX"
	}) {
		t.Fatalf("still transmitting 3 s after the rigctl keyer disconnected\n--- daemon log ---\n%s", d.Log())
	}
}

// A malformed command gets an error reply, and the connection stays usable.
func TestBadCommands(t *testing.T) {
	d := startDaemon(t, daemonOpts{})
	ws := dialRadiod(t, d)

	if err := ws.ws.writeFrame(opText, []byte(`{"nocmd":1}`)); err != nil {
		t.Fatal(err)
	}
	r := ws.nextReply()
	if r.OK || r.Error != "missing cmd" {
		t.Errorf("reply to a message without cmd = %+v, want ok false, error \"missing cmd\"", r)
	}

	if r := ws.call("set_profile", map[string]any{"profile": 99}); r.OK {
		t.Errorf("set_profile 99 was accepted: %+v", r)
	}
	if got := ws.call("get_frequency", nil).Number(t); got == 0 {
		t.Errorf("get_frequency after bad commands = 0")
	}
}
