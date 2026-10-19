package radio_test

import (
	"fmt"
	"strings"
	"testing"
	"time"

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

func radioState(m map[string]any) bool { return m["type"] == "event" && m["topic"] == "radio.state" }

// radio.state carries getRadioState's data: the whole of it on subscribe,
// then what changed, whether the change came from a command on this
// connection or from elsewhere in the daemon (CAT, rigctl, the front panel).
func TestRadioStateTopic(t *testing.T) {
	e := testapi.New(t)
	w := e.Open()
	if h := w.Next(testapi.Type("hello"), time.Second); !contains(h["topics"], "radio.state") {
		t.Fatalf("hello topics = %v", h["topics"])
	}
	w.Send(`{"type":"subscribe","id":"s","topics":["radio.state"]}`)
	snap := w.Next(radioState, time.Second)
	rest := e.Do("GET", "/radio", "").JSON(t)
	data := snap["data"].(map[string]any)
	for k, v := range rest {
		if data[k] != v {
			t.Errorf("snapshot %s = %v, GET /radio says %v", k, data[k], v)
		}
	}

	w.Send(`{"type":"cmd","id":"c","cmd":"radio.set","args":{"frequency_hz":7074000}}`)
	if r := w.Next(testapi.Type("reply"), time.Second); r["ok"] != true {
		t.Fatalf("radio.set = %v", r)
	}
	ev := w.Next(radioState, time.Second)
	if d := ev["data"].(map[string]any); d["frequency_hz"] != 7074000.0 || len(d) != 1 || ev["seq"] != 1.0 {
		t.Errorf("after radio.set: %v, want seq 1 with frequency_hz only", ev)
	}

	e.Radio.SetTX(true) // a change from elsewhere in the daemon
	ev = w.Next(radioState, time.Second)
	if d := ev["data"].(map[string]any); d["tx"] != true || ev["seq"] != 2.0 {
		t.Errorf("after keying: %v", ev)
	}
}

func contains(list any, s string) bool {
	l, _ := list.([]any)
	for _, v := range l {
		if v == s {
			return true
		}
	}
	return false
}

// /radio/settings: what radiod keeps once for the station. On a Hamlib radio
// there is no BFO (null, and refused); there is no transmit timeout yet.
func TestRadioSettings(t *testing.T) {
	e := testapi.New(t)
	r := e.Do("GET", "/radio/settings", "")
	j := r.JSON(t)
	if r.Status != 200 || j["default_profile"] != 0.0 || j["default_profile_fallback_s"] != nil ||
		j["step_hz"] != 100.0 || j["swr_limit"] != 2.5 || j["bfo_hz"] != nil || j["tx_timeout_s"] != nil {
		t.Fatalf("GET /radio/settings = %d %s", r.Status, r.Body)
	}
	for _, k := range []string{"default_profile", "default_profile_fallback_s", "step_hz", "swr_limit", "bfo_hz", "tx_timeout_s"} {
		if _, ok := j[k]; !ok {
			t.Errorf("settings have no %s: %s", k, r.Body)
		}
	}

	r = e.Do("PATCH", "/radio/settings", `{"default_profile":1,"default_profile_fallback_s":300,"step_hz":500,"swr_limit":3.0}`)
	j = r.JSON(t)
	if r.Status != 200 || j["default_profile"] != 1.0 || j["default_profile_fallback_s"] != 300.0 ||
		j["step_hz"] != 500.0 || j["swr_limit"] != 3.0 {
		t.Fatalf("PATCH = %d %s", r.Status, r.Body)
	}
	if s := e.Radio.Settings(); s.SWRLimitX10 != 30 || s.DefaultProfileFallback != 300 {
		t.Errorf("the radio got %+v", s)
	}
	if j = e.Do("PATCH", "/radio/settings", `{"default_profile_fallback_s":null}`).JSON(t); j["default_profile_fallback_s"] != nil {
		t.Errorf("null fallback: %v", j)
	}
	if s := e.Radio.Settings(); s.DefaultProfileFallback != -1 {
		t.Errorf("null fallback reached the radio as %d, want -1", s.DefaultProfileFallback)
	}
}

// Every field is checked before any is applied; a refused PATCH changes nothing.
func TestRadioSettingsRefused(t *testing.T) {
	e := testapi.New(t)
	cases := map[string]string{
		`{"step_hz":250,"swr_limit":2.55}`:   "swr_limit",
		`{"step_hz":250,"swr_limit":0.9}`:    "swr_limit",
		`{"step_hz":0}`:                      "step_hz",
		`{"default_profile":2}`:              "default_profile",
		`{"default_profile_fallback_s":0}`:   "default_profile_fallback_s",
		`{"bfo_hz":40036000}`:                "bfo_hz",
		`{"tx_timeout_s":60}`:                "tx_timeout_s",
		`{"step_hz":250,"ref_threshold":25}`: "ref_threshold",
	}
	for body, field := range cases {
		r := e.Do("PATCH", "/radio/settings", body)
		j := r.JSON(t)
		errs, _ := j["errors"].(map[string]any)
		if r.Status != 422 || j["code"] != "invalid" || errs[field] == nil {
			t.Errorf("PATCH %s = %d %s, want 422 on %s", body, r.Status, r.Body, field)
		}
	}
	if s := e.Radio.Settings(); s.StepHz != 100 {
		t.Errorf("a refused PATCH changed the step to %d", s.StepHz)
	}
}

// On an sBitx the BFO is reported and can be set.
func TestRadioSettingsBfo(t *testing.T) {
	e := testapi.New(t)
	e.Radio.UseSBitx()
	if j := e.Do("GET", "/radio/settings", "").JSON(t); j["bfo_hz"] != 40035000.0 {
		t.Fatalf("sBitx BFO = %v", j["bfo_hz"])
	}
	r := e.Do("PATCH", "/radio/settings", `{"bfo_hz":40036000}`)
	if r.Status != 200 || r.JSON(t)["bfo_hz"] != 40036000.0 {
		t.Errorf("PATCH bfo_hz = %d %s", r.Status, r.Body)
	}
}

// Profiles no longer carry station-wide values, and refuse them with a
// pointer to /radio/settings (PATCH and radio.set alike).
func TestProfilesWithoutStationSettings(t *testing.T) {
	e := testapi.New(t)
	p := e.Do("GET", "/radio/profiles/0", "").JSON(t)
	for _, k := range []string{"step_hz", "bfo_hz", "ref_threshold", "timeout_s", "name", "tone_hz"} {
		if _, has := p[k]; has {
			t.Errorf("profile has %s: %v", k, p)
		}
	}
	r := e.Do("PATCH", "/radio/profiles/0", `{"power":20,"step_hz":500}`)
	j := r.JSON(t)
	errs, _ := j["errors"].(map[string]any)
	if r.Status != 422 || !strings.Contains(fmt.Sprint(errs["step_hz"]), "/radio/settings") {
		t.Errorf("PATCH profile step_hz = %d %s", r.Status, r.Body)
	}
	if prof := e.Do("GET", "/radio/profiles/0", "").JSON(t); prof["power"] != 10.0 {
		t.Errorf("a refused PATCH changed power to %v", prof["power"])
	}
	if rep := e.Cmd("radio.set", `{"bfo_hz":40036000}`); rep["ok"] != false {
		t.Errorf("radio.set bfo_hz = %v", rep)
	}
}
