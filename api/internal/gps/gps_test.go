package gps_test

import (
	"testing"
	"time"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
)

func TestGetGps(t *testing.T) {
	e := testapi.New(t)
	r := e.Do("GET", "/gps", "")
	if r.Status != 200 {
		t.Fatalf("GET /gps = %d %s", r.Status, r.Body)
	}
	j := r.JSON(t)
	if j["fix"] != "none" {
		t.Errorf("fix = %v, want none", j["fix"])
	}
	s := j["settings"].(map[string]any)
	if s["sample_interval_s"].(float64) < 1 || s["file_period_s"].(float64) < 60 {
		t.Errorf("settings = %v", s)
	}
	if s["email"] != nil {
		t.Errorf("email = %v, want null", s["email"])
	}
}

func TestUpdateGpsSettings(t *testing.T) {
	e := testapi.New(t)
	r := e.Do("PATCH", "/gps/settings", `{"logging":true,"sample_interval_s":5,"email":"gps@example.com"}`)
	if r.Status != 200 {
		t.Fatalf("PATCH = %d %s", r.Status, r.Body)
	}
	s := r.JSON(t)
	if s["logging"] != true || s["sample_interval_s"].(float64) != 5 || s["email"] != "gps@example.com" {
		t.Errorf("settings = %v", s)
	}
	// Fields not in the patch keep their values.
	if s["file_period_s"].(float64) < 60 {
		t.Errorf("file_period_s = %v", s["file_period_s"])
	}
	// A null email clears it.
	r = e.Do("PATCH", "/gps/settings", `{"email":null}`)
	if r.Status != 200 || r.JSON(t)["email"] != nil {
		t.Errorf("email null = %d %s", r.Status, r.Body)
	}
}

func TestUpdateGpsSettingsInvalid(t *testing.T) {
	e := testapi.New(t)
	cases := []struct{ body, field string }{
		{`{"logging":1}`, "logging"},
		{`{"sample_interval_s":0}`, "sample_interval_s"},
		{`{"file_period_s":30}`, "file_period_s"},
		{`{"email":42}`, "email"},
		{`{"email":"no-at-sign"}`, "email"},
		{`{"report":{"interval_s":1}}`, "report.interval_s"},
		{`{"report":null}`, "report"},
		{`{"nonsense":1}`, "nonsense"},
	}
	for _, tc := range cases {
		r := e.Do("PATCH", "/gps/settings", tc.body)
		if r.Status != 422 {
			t.Errorf("PATCH %s = %d, want 422 (%s)", tc.body, r.Status, r.Body)
			continue
		}
		if _, ok := r.JSON(t)["errors"].(map[string]any)[tc.field]; !ok {
			t.Errorf("PATCH %s: no error for %q: %s", tc.body, tc.field, r.Body)
		}
	}
}

// A refused write changes nothing.
func TestUpdateGpsSettingsRefused(t *testing.T) {
	e := testapi.New(t)
	r := e.Do("PATCH", "/gps/settings", `{"sample_interval_s":5,"file_period_s":1}`)
	if r.Status != 422 {
		t.Fatalf("PATCH = %d %s", r.Status, r.Body)
	}
	g := e.Do("GET", "/gps", "").JSON(t)
	if s := g["settings"].(map[string]any); s["sample_interval_s"].(float64) == 5 {
		t.Errorf("a refused patch changed sample_interval_s: %v", s)
	}
}

func TestGpsTopic(t *testing.T) {
	e := testapi.New(t)
	w := e.Open()
	w.Send(`{"type":"subscribe","id":"s","topics":["gps"]}`)
	isGps := func(m map[string]any) bool { return m["type"] == "event" && m["topic"] == "gps" }

	snap := w.Next(isGps, time.Second)
	d := snap["data"].(map[string]any)
	if d["change"] != "self" {
		t.Errorf("snapshot change = %v", d["change"])
	}
	if d["gps"] == nil {
		t.Fatalf("snapshot has no gps: %v", snap)
	}

	// A fix arrives from elsewhere; subscribers get only what changed.
	lat, lon := 19.43, -99.13
	e.Radio.SetGpsFix("3d", &lat, &lon, nil, nil)
	ev := w.Next(isGps, time.Second)
	if ev["data"].(map[string]any)["gps"] == nil {
		t.Fatalf("no gps in event: %v", ev)
	}
}
