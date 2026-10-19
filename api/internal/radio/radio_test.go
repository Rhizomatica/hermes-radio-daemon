package radio_test

import (
	"testing"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
)

func TestGetStateAndProfiles(t *testing.T) {
	e := testapi.New(t)
	j := e.Do("GET", "/radio", "").JSON(t)
	if j["frequency_hz"] != 7100000.0 || j["mode"] != "USB" || j["backend"] != "hamlib" || j["swr"] != nil {
		t.Errorf("state = %v (swr is null while receiving)", j)
	}
	if _, has := j["timeout_s"]; has {
		t.Error("timeout_s is reported (radiod has no transmit timeout)")
	}
	data := e.Do("GET", "/radio/profiles", "").JSON(t)["data"].([]any)
	if len(data) != 2 || data[1].(map[string]any)["frequency_hz"] != 14200000.0 {
		t.Errorf("profiles = %v", data)
	}
	if r := e.Do("GET", "/radio/profiles/7", ""); r.Status != 404 {
		t.Errorf("profile 7 = %d", r.Status)
	}
}

// A patch is validated in full before anything is applied.
func TestUpdateProfile(t *testing.T) {
	e := testapi.New(t)
	r := e.Do("PATCH", "/radio/profiles/1", `{"frequency_hz":14070000,"mode":"lsb","power":30,"filter_width_hz":2400}`)
	if r.Status != 200 {
		t.Fatalf("PATCH = %d %s", r.Status, r.Body)
	}
	j := r.JSON(t)
	if j["frequency_hz"] != 14070000.0 || j["mode"] != "LSB" || j["power"] != 30.0 || j["filter_width_hz"] != 2400.0 {
		t.Errorf("profile = %v", j)
	}
	r = e.Do("PATCH", "/radio/profiles/1", `{"frequency_hz":7000000,"power":300,"timeout_s":10,"name":"x","bogus":1}`)
	if r.Status != 422 {
		t.Fatalf("bad PATCH = %d %s", r.Status, r.Body)
	}
	errs := r.JSON(t)["errors"].(map[string]any)
	for _, f := range []string{"power", "timeout_s", "name", "bogus"} {
		if errs[f] == nil {
			t.Errorf("no error for %s: %v", f, errs)
		}
	}
	if p, _ := e.Radio.Profile(1); p.FrequencyHz != 14070000 {
		t.Errorf("a refused patch changed frequency_hz to %d", p.FrequencyHz)
	}
	if r := e.Do("PATCH", "/radio/profiles/1", `{"filter_width_hz":null}`); r.JSON(t)["filter_width_hz"] != nil {
		t.Errorf("null filter width (the rig's default) = %s", r.Body)
	}
}

func TestSelectProfileAndProtection(t *testing.T) {
	e := testapi.New(t)
	if j := e.Do("PUT", "/radio/active-profile", `{"profile":1}`).JSON(t); j["active_profile"] != 1.0 {
		t.Errorf("select = %v", j)
	}
	if r := e.Do("PUT", "/radio/active-profile", `{"profile":9}`); r.Status != 422 {
		t.Errorf("select 9 = %d", r.Status)
	}
	e.Radio.SetTX(true)
	if r := e.Do("PUT", "/radio/active-profile", `{"profile":0}`); r.Status != 409 || r.JSON(t)["code"] != "busy" {
		t.Errorf("select while transmitting = %d %s", r.Status, r.Body)
	}
	e.Radio.SetTX(false)

	e.Radio.TripProtection()
	if r := e.Do("POST", "/radio/protection/reset", ""); r.Status != 204 || len(r.Body) != 0 {
		t.Errorf("reset = %d %q", r.Status, r.Body)
	}
	if e.Radio.State().Protection {
		t.Error("protection still tripped")
	}
}

func TestCommands(t *testing.T) {
	e := testapi.New(t)
	r := e.Cmd("radio.set", `{"frequency_hz":7074000,"mode":"USB"}`)
	if r["ok"] != true || r["data"].(map[string]any)["frequency_hz"] != 7074000.0 {
		t.Errorf("radio.set = %v", r)
	}
	r = e.Cmd("radio.set", `{"profile":1,"power":101}`)
	if r["ok"] != false || r["error"].(map[string]any)["code"] != "out_of_range" {
		t.Errorf("radio.set out of range = %v", r)
	}
	if p, _ := e.Radio.Profile(1); p.Power != 100 {
		t.Errorf("a refused radio.set changed power to %d", p.Power)
	}
	if r = e.Cmd("radio.select_profile", `{}`); r["error"].(map[string]any)["code"] != "missing_arg" {
		t.Errorf("select without profile = %v", r)
	}
	if r = e.Cmd("radio.select_profile", `{"profile":1}`); r["ok"] != true {
		t.Errorf("select 1 = %v", r)
	}
	if r = e.Cmd("radio.reset_protection", ""); r["ok"] != true {
		t.Errorf("reset_protection = %v", r)
	}
}
