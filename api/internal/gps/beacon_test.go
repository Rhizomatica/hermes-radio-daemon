package gps_test

import (
	"os"
	"path/filepath"
	"testing"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/gps"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
)

func running(e *testapi.Env) {
	e.Radio.SetBroadcast(rad.Broadcast{Running: true, Connected: true, ModeName: "DATAC3"}, nil)
}

func TestBeaconSettings(t *testing.T) {
	e := testapi.New(t)
	j := e.Do("GET", "/gps/beacon", "").JSON(t)
	if j["enabled"] != false || j["interval_s"].(float64) != 900 ||
		j["callsign"] != "" || j["station_latitude"] != nil || j["emergency_interval_s"].(float64) != 120 || j["running"] != false {
		t.Fatalf("GET /gps/beacon = %v", j)
	}

	r := e.Do("PATCH", "/gps/beacon", `{"enabled":true,"interval_s":300,"callsign":"n0call-7","station_latitude":4.5,"station_longitude":-75.7,"emergency_interval_s":60}`)
	if r.Status != 200 {
		t.Fatalf("PATCH = %d %s", r.Status, r.Body)
	}
	j = r.JSON(t)
	if j["enabled"] != true || j["interval_s"].(float64) != 300 ||
		j["callsign"] != "N0CALL-7" || j["station_latitude"] != 4.5 || j["station_longitude"] != -75.7 || j["emergency_interval_s"].(float64) != 60 {
		t.Errorf("after PATCH = %v", j)
	}

	// The fixed position goes away with both coordinates null.
	j = e.Do("PATCH", "/gps/beacon", `{"station_latitude":null,"station_longitude":null}`).JSON(t)
	if j["station_latitude"] != nil || j["station_longitude"] != nil || j["callsign"] != "N0CALL-7" {
		t.Errorf("after clearing the position = %v", j)
	}

	for _, bad := range []string{
		`{"callsign":"N0 CALL"}`, `{"callsign":"TOOLONGCALL"}`,
		`{"station_latitude":91,"station_longitude":0}`, `{"station_latitude":1}`,
		`{"emergency_interval_s":10}`, `{"interval_s":59}`, `{"enabled":"yes"}`, `{"nonsense":1}`,
	} {
		if r := e.Do("PATCH", "/gps/beacon", bad); r.Status != 422 {
			t.Errorf("PATCH %s = %d, want 422", bad, r.Status)
		}
	}
}

func TestBeaconSendAndSos(t *testing.T) {
	e := testapi.New(t)
	if r := e.Do("POST", "/gps/beacon/send", ""); r.Status != 503 {
		t.Errorf("send with the plane off = %d, want 503", r.Status)
	}
	if r := e.Do("POST", "/gps/sos", `{"code":2}`); r.Status != 503 {
		t.Errorf("SOS with the plane off = %d, want 503: nothing would go out", r.Status)
	}

	running(e)
	if r := e.Do("POST", "/gps/beacon/send", ""); r.Status != 202 || e.Radio.BeaconSends() != 1 {
		t.Errorf("send = %d, sends %d", r.Status, e.Radio.BeaconSends())
	}
	r := e.Do("POST", "/gps/sos", "")
	if r.Status != 202 || r.JSON(t)["emergency"].(float64) != gps.SosDefault {
		t.Fatalf("SOS without a code = %d %s", r.Status, r.Body)
	}
	if r := e.Do("POST", "/gps/sos", `{"code":2}`); r.Status != 202 || r.JSON(t)["emergency"].(float64) != 2 {
		t.Errorf("SOS medical = %d %s", r.Status, r.Body)
	}
	for _, bad := range []string{`{"code":0}`, `{"code":31}`, `{"code":"x"}`} {
		if r := e.Do("POST", "/gps/sos", bad); r.Status != 422 && r.Status != 400 {
			t.Errorf("SOS %s = %d, want 422", bad, r.Status)
		}
	}
	if r := e.Do("DELETE", "/gps/sos", ""); r.Status != 200 || r.JSON(t)["emergency"].(float64) != 0 {
		t.Errorf("clear = %d %s", r.Status, r.Body)
	}
}

func TestBeaconsAndStations(t *testing.T) {
	e := testapi.New(t)
	alt := 760
	e.Radio.SetBeaconStations([]rad.BeaconStation{
		{Callsign: "B0AB", Latitude: 4.5, Longitude: -75.7, Fix: 3, HeardAt: "2026-11-06T12:00:00Z"},
		{Callsign: "N0CALL", Latitude: -23.5, Longitude: -46.6, Fix: 2, AltitudeM: &alt, Emergency: 8, HeardAt: "2026-11-06T11:00:00Z"},
	})
	j := e.Do("GET", "/gps/beacons", "").JSON(t)
	data := j["data"].([]any)
	if len(data) != 2 || data[0].(map[string]any)["callsign"] != "B0AB" || data[1].(map[string]any)["emergency"].(float64) != 8 {
		t.Fatalf("GET /gps/beacons = %v", j)
	}

	sys := filepath.Join(t.TempDir(), "sys")
	os.WriteFile(sys, []byte("# stations\nsystem b0ab\nport tcp\n\nsystem B0AC # never heard\n"), 0o644)
	old := gps.UUCPSys
	gps.UUCPSys = sys
	t.Cleanup(func() { gps.UUCPSys = old })

	j = e.Do("GET", "/gps/stations", "").JSON(t)
	st := j["data"].([]any)
	if len(st) != 2 {
		t.Fatalf("GET /gps/stations = %v", j)
	}
	a, b := st[0].(map[string]any), st[1].(map[string]any)
	if a["uucp_name"] != "b0ab" || a["latitude"] != 4.5 || a["position_source"] != "manual" || a["position_at"] != "2026-11-06T12:00:00Z" {
		t.Errorf("heard station = %v", a)
	}
	if b["uucp_name"] != "B0AC" || b["latitude"] != nil || b["position_source"] != nil {
		t.Errorf("station never heard = %v", b)
	}
}
