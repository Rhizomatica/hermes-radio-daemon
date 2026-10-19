package modem_test

import (
	"testing"

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
