package integration

import (
	"bytes"
	"encoding/binary"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"testing"
	"time"
)

// KISS framing, as Mercury's broadcast port speaks it.
const (
	kissFEND  = 0xC0
	kissFESC  = 0xDB
	kissTFEND = 0xDC
	kissTFESC = 0xDD
	kissModem = 0x03 // CMD_MODEM_FRAME
)

func kissEncode(frame []byte) []byte {
	out := []byte{kissFEND, kissModem}
	for _, b := range frame {
		switch b {
		case kissFEND:
			out = append(out, kissFESC, kissTFEND)
		case kissFESC:
			out = append(out, kissFESC, kissTFESC)
		default:
			out = append(out, b)
		}
	}
	return append(out, kissFEND)
}

// fakeMercury is Mercury's broadcast port: it hands every frame radiod sends
// to frames, and writes what the test injects.
type fakeMercury struct {
	ln     net.Listener
	frames chan []byte
	conn   chan net.Conn
}

func newFakeMercury(t *testing.T) *fakeMercury {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	m := &fakeMercury{ln: ln, frames: make(chan []byte, 4096), conn: make(chan net.Conn, 4)}
	t.Cleanup(func() { ln.Close() })
	go func() {
		for {
			c, err := ln.Accept()
			if err != nil {
				return
			}
			m.conn <- c
			go m.read(c)
		}
	}()
	return m
}

func (m *fakeMercury) port() int { return m.ln.Addr().(*net.TCPAddr).Port }

func (m *fakeMercury) read(c net.Conn) {
	var cur []byte
	in, esc := false, false
	buf := make([]byte, 4096)
	for {
		n, err := c.Read(buf)
		if err != nil {
			return
		}
		for _, b := range buf[:n] {
			switch {
			case b == kissFEND:
				if in && len(cur) > 1 {
					m.frames <- append([]byte(nil), cur[1:]...) // past the command
				}
				cur, in, esc = cur[:0], true, false
			case !in:
			case esc:
				if b == kissTFEND {
					b = kissFEND
				} else if b == kissTFESC {
					b = kissFESC
				}
				cur, esc = append(cur, b), false
			case b == kissFESC:
				esc = true
			default:
				cur = append(cur, b)
			}
		}
	}
}

// next is the next frame whose first byte's packet type is ptype.
func (m *fakeMercury) next(t *testing.T, ptype byte, within time.Duration) []byte {
	t.Helper()
	deadline := time.After(within)
	for {
		select {
		case f := <-m.frames:
			if f[0]>>5 == ptype {
				return f
			}
		case <-deadline:
			t.Fatalf("no frame of packet type %#x within %v", ptype, within)
			return nil
		}
	}
}

// beaconAlphabet is the beacon's base-40 callsign alphabet (docs/BEACON-FORMAT.md §5).
const beaconAlphabet = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-/."

func callsignOf(b []byte) string {
	v := uint64(0)
	for i := 5; i >= 0; i-- {
		v = v<<8 | uint64(b[i])
	}
	s := ""
	for ; v > 0; v /= 40 {
		s += string(beaconAlphabet[v%40])
	}
	return s
}

// compactBeacon is a 14-byte compact beacon (§3).
func compactBeacon(call string, lat, lon float64, code uint64) []byte {
	v := uint64(0)
	for i := len(call) - 1; i >= 0; i-- {
		v = v*40 + uint64(bytes.IndexByte([]byte(beaconAlphabet), call[i]))
	}
	f := make([]byte, 14)
	f[0] = 0xE0
	binary.LittleEndian.PutUint64(f[1:9], v) // 6 bytes; the next two are overwritten
	la := uint64((lat+90)*1e5 + 0.5)
	lo := uint64((lon+180)*1e5 + 0.5)
	p := la | lo<<25 | code<<51
	for i := 0; i < 7; i++ {
		f[7+i] = byte(p >> (8 * i))
	}
	return f
}

func upload(t *testing.T, url, token string, body []byte) httpResult {
	t.Helper()
	req, _ := http.NewRequest("POST", url, bytes.NewReader(body))
	req.Header.Set("Content-Type", "application/octet-stream")
	req.Header.Set("Authorization", "Bearer "+token)
	resp, err := (&http.Client{Timeout: 5 * time.Second}).Do(req)
	if err != nil {
		t.Fatalf("POST %s: %v", url, err)
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	return httpResult{status: resp.StatusCode, header: resp.Header, raw: string(b)}
}

// The broadcast plane through the API, against a fake Mercury: a file queued
// goes out as carousel frames, a beacon sent goes out as a station frame with
// the configured callsign, and a beacon Mercury hears is listed.
func TestBroadcastPlaneThroughTheAPI(t *testing.T) {
	needGoAPI(t)
	m := newFakeMercury(t)
	spool := t.TempDir()
	d := startAPIDaemon(t,
		"broadcast_enable = 1",
		"broadcast_port = "+strconv.Itoa(m.port()),
		"broadcast_tx_dir = "+filepath.Join(spool, "tx"),
		"broadcast_rx_dir = "+filepath.Join(spool, "rx"),
		"callsign = N0CALL",
		"station_latitude = 51.4779",
		"station_longitude = -0.0015")
	admin := setupAPI(t, d)
	base := apiBase(d)

	var conn net.Conn
	select {
	case conn = <-m.conn:
	case <-time.After(10 * time.Second):
		t.Fatalf("radiod did not connect to the broadcast port\n%s", d.Log())
	}

	r := callAuth(t, "GET", base+"/broadcasts", "", admin)
	if r.status != 200 || r.body["running"] != true || r.body["mode"] != "DATAC3" {
		t.Fatalf("GET /broadcasts = %d %s", r.status, r.raw)
	}

	// A file queued goes out as DATAC3 carousel frames.
	r = upload(t, base+"/broadcasts?name=bulletin.txt&passes=1", admin, bytes.Repeat([]byte("hermes "), 100))
	if r.status != 201 {
		t.Fatalf("POST /broadcasts = %d %s", r.status, r.raw)
	}
	f := m.next(t, 0x03, 10*time.Second)
	if len(f) != 126 {
		t.Errorf("a carousel frame of %d bytes, want DATAC3's 126", len(f))
	}

	// A beacon sent now: the compact layout with the station's callsign.
	if r := callAuth(t, "POST", base+"/gps/beacon/send", "", admin); r.status != 202 {
		t.Fatalf("POST /gps/beacon/send = %d %s", r.status, r.raw)
	}
	b := m.next(t, 0x07, 10*time.Second)
	if len(b) != 14 || b[0] != 0xE0 || callsignOf(b[1:7]) != "N0CALL" {
		t.Errorf("beacon frame % x (callsign %q)", b, callsignOf(b[1:7]))
	}

	// A beacon Mercury hears is listed, and the map's beacon log has it.
	if _, err := conn.Write(kissEncode(compactBeacon("B0AB", 4.5, -75.7, 8))); err != nil {
		t.Fatal(err)
	}
	var heard []any
	for i := 0; i < 50 && len(heard) == 0; i++ {
		heard, _ = callAuth(t, "GET", base+"/gps/beacons", "", admin).body["data"].([]any)
		if len(heard) == 0 {
			time.Sleep(100 * time.Millisecond)
		}
	}
	if len(heard) != 1 || heard[0].(map[string]any)["callsign"] != "B0AB" || heard[0].(map[string]any)["emergency"] != float64(8) {
		t.Fatalf("GET /gps/beacons = %v", heard)
	}

	// A file without a budget, as NNCP drops them, goes out until it is
	// taken out; the API lists it on the air and takes it out.
	if err := os.WriteFile(filepath.Join(spool, "tx", "nncp-bundle.bin"), bytes.Repeat([]byte{0x5A}, 2000), 0o644); err != nil {
		t.Fatal(err)
	}
	var onAir map[string]any
	for i := 0; i < 50 && onAir == nil; i++ {
		q, _ := callAuth(t, "GET", base+"/broadcasts", "", admin).body["data"].([]any)
		for _, e := range q {
			if e := e.(map[string]any); e["id"] == "nncp-bundle.bin" && e["state"] == "sending" {
				onAir = e
			}
		}
		if onAir == nil {
			time.Sleep(100 * time.Millisecond)
		}
	}
	if onAir == nil || onAir["frames_limit"] != nil {
		t.Fatalf("the spool file on the air = %v", onAir)
	}
	if r := callAuth(t, "DELETE", base+"/broadcasts/nncp-bundle.bin", "", admin); r.status != 204 {
		t.Fatalf("DELETE = %d %s", r.status, r.raw)
	}
	if q, _ := callAuth(t, "GET", base+"/broadcasts", "", admin).body["data"].([]any); len(q) != 0 {
		t.Errorf("queue after DELETE = %v", q)
	}
}

// With the broadcast plane off: beacons can be configured, but nothing that
// needs the plane pretends to work.
func TestBroadcastPlaneOff(t *testing.T) {
	needGoAPI(t)
	d := startAPIDaemon(t)
	admin := setupAPI(t, d)
	base := apiBase(d)
	wantStatus(t, d, "GET /broadcasts", callAuth(t, "GET", base+"/broadcasts", "", admin), 503, "not_running")
	wantStatus(t, d, "SOS", callAuth(t, "POST", base+"/gps/sos", `{"code":1}`, admin), 503, "not_running")
	r := callAuth(t, "PATCH", base+"/gps/beacon", `{"callsign":"N0CALL","emergency_interval_s":60}`, admin)
	if r.status != 200 || r.body["callsign"] != "N0CALL" || r.body["running"] != false {
		t.Fatalf("PATCH /gps/beacon = %d %s", r.status, r.raw)
	}
}
