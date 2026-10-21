package modem_test

import (
	"fmt"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
)

func TestModemStatus(t *testing.T) {
	e := testapi.New(t)
	if r := e.Do("GET", "/modem", ""); r.Status != 503 || r.JSON(t)["code"] != "not_running" {
		t.Fatalf("without mercury = %d %s", r.Status, r.Body)
	}
	e.Radio.SetModem(`{"type":"status","bitrate":2400,"snr":7.5,"user_callsign":"PU2UIT","dest_callsign":"PY2XYZ",` +
		`"sync":true,"direction":"rx","bytes_transmitted":10,"bytes_received":20,"tx_gain_db":3,"tx_peak_dbfs":-6.5,` +
		`"audio_ok":true,"audio_error":"","arq_tx_mode":"DATAC1","arq_rx_mode":"","peer_snr":-99.9,"peer_snr_valid":false}`)
	j := e.Do("GET", "/modem", "").JSON(t)
	want := map[string]any{"link": "connected", "my_call": "PU2UIT", "peer_call": "PY2XYZ", "direction": "rx",
		"snr_db": 7.5, "bitrate_bps": 2400.0, "bytes_tx": 10.0, "bytes_rx": 20.0, "arq_tx_mode": "DATAC1",
		"arq_rx_mode": nil, "peer_snr_db": nil, "audio_ok": true, "audio_error": nil}
	for k, v := range want {
		if j[k] != v {
			t.Errorf("%s = %v, want %v", k, j[k], v)
		}
	}
	if e.Do("GET", "/health", "").JSON(t)["components"].(map[string]any)["modem"] != "ok" {
		t.Error("health does not report the modem")
	}
}

func mercuryStatus(bytesRx int) string {
	return `{"type":"status","bitrate":980,"snr":12.5,"user_callsign":"N0CALL","dest_callsign":"N1CALL",` +
		`"sync":true,"direction":"rx","bytes_transmitted":0,"bytes_received":` + strconv.Itoa(bytesRx) +
		`,"tx_gain_db":0,"tx_peak_dbfs":-6,"audio_ok":true,"audio_error":"","arq_tx_mode":"","arq_rx_mode":"DATAC1",` +
		`"peer_snr":-99.9,"peer_snr_valid":false}`
}

func modemStatus(m map[string]any) bool { return m["type"] == "event" && m["topic"] == "modem.status" }

// modem.status: offered only while mercury runs; its snapshot is GET /modem,
// and an event carries what changed in mercury's status.
func TestModemStatusTopic(t *testing.T) {
	e := testapi.New(t)
	w := e.Open()
	if h := w.Next(testapi.Type("hello"), time.Second); strings.Contains(fmt.Sprint(h["topics"]), "modem.status") {
		t.Errorf("hello offers modem.status without mercury: %v", h["topics"])
	}
	w.Send(`{"type":"subscribe","id":"s","topics":["modem.status"]}`)
	r := w.Next(testapi.Type("reply"), time.Second)
	if er, _ := r["error"].(map[string]any); r["ok"] != false || er["code"] != "not_supported" {
		t.Fatalf("subscribe without mercury = %v", r)
	}

	e.Radio.SetModem(mercuryStatus(100))
	w = e.Open()
	if h := w.Next(testapi.Type("hello"), time.Second); !strings.Contains(fmt.Sprint(h["topics"]), "modem.status") {
		t.Errorf("hello does not offer modem.status with mercury running: %v", h["topics"])
	}
	w.Send(`{"type":"subscribe","id":"s","topics":["modem.status"]}`)
	snap := w.Next(modemStatus, time.Second)
	rest := e.Do("GET", "/modem", "").JSON(t)
	data := snap["data"].(map[string]any)
	for k, v := range rest {
		if data[k] != v {
			t.Errorf("snapshot %s = %v, GET /modem says %v", k, data[k], v)
		}
	}
	if data["link"] != "connected" || data["peer_call"] != "N1CALL" {
		t.Errorf("snapshot = %v", data)
	}

	e.Radio.SetModem(mercuryStatus(2048))
	ev := w.Next(modemStatus, time.Second)
	if d := ev["data"].(map[string]any); d["bytes_rx"] != 2048.0 || len(d) != 1 || ev["seq"] != 1.0 {
		t.Errorf("event = %v, want seq 1 with bytes_rx only", ev)
	}

	e.Radio.SetModem("") // mercury stops: no events, and no bogus "everything changed"
	w.None(modemStatus, 300*time.Millisecond)
}
