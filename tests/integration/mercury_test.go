// Mercury linked into radiod (main:enable_mercury): it starts with the
// daemon, serves its TNC ports, and stops with it.  Skipped when the
// binary was built without the vendor/mercury submodule.
//
// Copyright (C) 2026 Rhizomatica
// SPDX-License-Identifier: GPL-3.0-or-later

package integration

import (
	"bufio"
	"fmt"
	"net"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

// startMercuryDaemon starts radiod with mercury linked in, on ALSA's null
// device, its TNC on free ports.  Returns the daemon and the TNC control
// address.
func startMercuryDaemon(t *testing.T) (*Daemon, string) {
	t.Helper()
	if !hasMercury(t) {
		t.Skip("radio_daemon was built without mercury (vendor/mercury not checked out)")
	}
	ctl, bcast := freePort(t), freePort(t)
	for bcast == ctl+1 {
		bcast = freePort(t) // ctl+1 is the TNC data port
	}
	ini := filepath.Join(t.TempDir(), "mercury.ini")
	writeFile(t, ini, fmt.Sprintf(`[main]
arq_tcp_base_port = %d
broadcast_tcp_port = %d
[ptt]
method = none
[store]
enabled = false
`, ctl, bcast))

	d := startDaemon(t, daemonOpts{core: []string{
		"enable_mercury = 1",
		"mercury_config = " + ini,
		"capture_device = null",
		"playback_device = null",
	}})
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
	for _, want := range []string{"mercury_link: mercury starting", "mercury_link: mercury stopped"} {
		if !strings.Contains(d.Log(), want) {
			t.Errorf("daemon log lacks %q\n--- daemon log ---\n%s", want, d.Log())
		}
	}
}
