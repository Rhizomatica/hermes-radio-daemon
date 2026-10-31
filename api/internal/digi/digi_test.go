package digi_test

import (
	"fmt"
	"net/url"
	"testing"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
)

func entries(e *testapi.Env) {
	for i := 1; i <= 5; i++ {
		mode, from := "FT8", "PU2UIT"
		if i%2 == 0 {
			mode, from = "CW", "PY2XYZ"
		}
		e.Radio.AddDigi(fmt.Sprintf(`{"id":%d,"at":"2026-10-19T10:0%d:00.000Z","mode":"%s","dir":"rx",`+
			`"frequency_hz":14074000,"text":"MSG %d","from":"%s","to":null}`, i, i, mode, i, from))
	}
}

// Newest first, cursor pages, filters.
func TestDigiLog(t *testing.T) {
	e := testapi.New(t)
	entries(e)
	j := e.Do("GET", "/digi/log?limit=2", "").JSON(t)
	data := j["data"].([]any)
	if len(data) != 2 || data[0].(map[string]any)["id"] != 5.0 || data[1].(map[string]any)["id"] != 4.0 {
		t.Fatalf("first page = %v", data)
	}
	next, _ := j["next_cursor"].(string)
	j = e.Do("GET", "/digi/log?limit=2&cursor="+url.QueryEscape(next), "").JSON(t)
	if data = j["data"].([]any); len(data) != 2 || data[0].(map[string]any)["id"] != 3.0 {
		t.Fatalf("second page = %v", data)
	}
	j = e.Do("GET", "/digi/log?mode=cw", "").JSON(t)
	if data = j["data"].([]any); len(data) != 2 {
		t.Errorf("mode=cw = %v", data)
	}
	if data = e.Do("GET", "/digi/log?callsign=pu2uit&since=2026-10-19T10:02:00Z", "").JSON(t)["data"].([]any); len(data) != 2 {
		t.Errorf("callsign and since = %v", data)
	}
	if r := e.Do("GET", "/digi/log?mode=PSK31&limit=500", ""); r.Status != 422 {
		t.Errorf("bad filters = %d", r.Status)
	}
	if r := e.Do("GET", "/digi/log?cursor=!!", ""); r.Status != 422 {
		t.Errorf("bad cursor = %d", r.Status)
	}
}

func TestDigiSettings(t *testing.T) {
	e := testapi.New(t)
	j := e.Do("GET", "/radio/digi", "").JSON(t)
	if j["cw"].(map[string]any)["wpm"] != 20.0 || j["voice_codec"] != "RADEV2" {
		t.Errorf("settings = %v", j)
	}
	r := e.Do("PATCH", "/radio/digi", `{"cw":{"wpm":25},"rtty":{"shift_hz":425}}`)
	if r.Status != 200 || r.JSON(t)["cw"].(map[string]any)["wpm"] != 25.0 {
		t.Errorf("patch = %d %s", r.Status, r.Body)
	}
	if r := e.Do("PATCH", "/radio/digi", `{"cw":{"wpm":99},"rtty":{"baud":110}}`); r.Status != 422 {
		t.Errorf("out of range = %d", r.Status)
	}
	if s := e.Radio.DigiSettings(); s.CWWPM != 25 || s.RTTYShiftHz != 425 {
		t.Errorf("a refused patch changed settings: %+v", s)
	}
	// Voice encryption is a setting like any other, on and off.
	r = e.Do("PATCH", "/radio/digi", `{"dstar":{"encrypt":true},"rade":{"encrypt":true}}`)
	if r.Status != 200 || !e.Radio.DigiSettings().DstarEncrypt || !e.Radio.DigiSettings().RadeEncrypt ||
		r.JSON(t)["rade"].(map[string]any)["encrypt"] != true {
		t.Errorf("encrypt on = %d %s", r.Status, r.Body)
	}
	r = e.Do("PATCH", "/radio/digi", `{"dstar":{"encrypt":false},"rade":{"encrypt":false}}`)
	if r.Status != 200 || e.Radio.DigiSettings().DstarEncrypt || e.Radio.DigiSettings().RadeEncrypt {
		t.Errorf("encrypt off = %d %s", r.Status, r.Body)
	}
	if r := e.Do("PATCH", "/radio/digi", `{"voice_codec":"DSTAR"}`); r.Status != 422 {
		t.Errorf("DSTAR codec on hamlib = %d, want 422", r.Status)
	}
}
