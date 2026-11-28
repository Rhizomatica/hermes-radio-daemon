package ale_test

import (
	"testing"
	"time"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
)

func running(e *testapi.Env) {
	e.Radio.SetAle(rad.AleState{Running: true, Mode: "monitor", State: "OFF", Self: "BOB",
		FrequencyHz: 7050000, USB: true, Channels: 2, Owner: "none"})
}

func errCode(r map[string]any) any {
	if e, ok := r["error"].(map[string]any); ok {
		return e["code"]
	}
	return nil
}

// Without ALE in radiod: the state says so, the actions are not_running.
func TestNotRunning(t *testing.T) {
	e := testapi.New(t)
	r := e.Do("GET", "/ale", "")
	if r.Status != 200 || r.JSON(t)["running"] != false || r.JSON(t)["state"] != "OFF" {
		t.Fatalf("GET /ale = %d %s", r.Status, r.Body)
	}
	if r := e.Do("POST", "/ale/start", `{"mode":"listen"}`); r.Status != 503 {
		t.Fatalf("start = %d %s, want 503", r.Status, r.Body)
	}
	if r := e.Cmd("ale.call", `{"to":"SAM"}`); errCode(r) != "not_supported" {
		t.Fatalf("ale.call = %v", r)
	}
	if r := e.Do("GET", "/ale/events", ""); r.Status != 200 {
		t.Fatalf("events = %d %s", r.Status, r.Body)
	}
}

func TestStartCallStop(t *testing.T) {
	e := testapi.New(t)
	running(e)
	if r := e.Do("POST", "/ale/call", `{"to":"SAM"}`); r.Status != 503 {
		t.Fatalf("call in monitor mode = %d %s, want 503", r.Status, r.Body)
	}
	r := e.Do("POST", "/ale/start", `{"mode":"scan"}`)
	if r.Status != 200 || r.JSON(t)["state"] != "SCANNING" || r.JSON(t)["owner"] != "ale" {
		t.Fatalf("start = %d %s", r.Status, r.Body)
	}
	if r := e.Cmd("ale.call", `{"to":"SAM","amd":"QSY 7102"}`); r["ok"] != true {
		t.Fatalf("ale.call = %v", r)
	}
	if r := e.Cmd("ale.call", `{"to":"SAM"}`); errCode(r) != "busy" {
		t.Fatalf("second call = %v, want busy", r)
	}
	if r := e.Do("POST", "/ale/terminate", ""); r.Status != 409 {
		t.Fatalf("terminate while calling = %d, want 409", r.Status)
	}
	r = e.Do("POST", "/ale/stop", "")
	if r.Status != 200 || r.JSON(t)["mode"] != "monitor" || r.JSON(t)["owner"] != "none" {
		t.Fatalf("stop = %d %s", r.Status, r.Body)
	}
}

// Validated in full before anything reaches radiod.
func TestValidation(t *testing.T) {
	e := testapi.New(t)
	running(e)
	for _, c := range []struct{ path, body string }{
		{"/ale/start", `{"mode":"sweep"}`},
		{"/ale/start", `{}`},
		{"/ale/call", `{"to":"sam"}`},
		{"/ale/call", `{"to":"ABCDEFGHIJKLMNOP"}`},
		{"/ale/call", `{"to":"SAM","amd":"lower case"}`},
		{"/ale/call", `{"to":"SAM","extra":1}`},
	} {
		if r := e.Do("POST", c.path, c.body); r.Status != 422 && r.Status != 400 {
			t.Errorf("POST %s %s = %d %s, want 422", c.path, c.body, r.Status, r.Body)
		}
	}
	if r := e.Cmd("ale.start", `{"mode":"sweep"}`); errCode(r) != "out_of_range" {
		t.Errorf("ale.start sweep = %v", r)
	}
	if r := e.Cmd("ale.call", `{}`); errCode(r) != "missing_arg" {
		t.Errorf("ale.call {} = %v", r)
	}
	if st := e.Radio.AleState(); st.Mode != "monitor" {
		t.Errorf("a refused action changed the state: %+v", st)
	}
}

func TestEventsAndTopics(t *testing.T) {
	e := testapi.New(t)
	running(e)
	e.Radio.AddAleEvent(rad.AleEvent{Kind: "word", Peer: "SAM", Combined: 4, Preamble: "TO", Unanimous: 41})
	e.Radio.AddAleEvent(rad.AleEvent{Kind: "linked", Peer: "SAM"})

	r := e.Do("GET", "/ale/events?limit=1", "")
	page := r.JSON(t)
	data, _ := page["data"].([]any)
	if r.Status != 200 || len(data) != 1 || page["next_cursor"] == nil {
		t.Fatalf("events page = %d %s", r.Status, r.Body)
	}
	first := data[0].(map[string]any)
	if first["kind"] != "word" || first["receiver"] != "combining" || first["combined"] != 4.0 || first["preamble"] != "TO" {
		t.Fatalf("word event = %v", first)
	}

	w := e.Open()
	w.Send(`{"type":"subscribe","id":"s","topics":["ale.state","ale.events"]}`)
	isState := func(m map[string]any) bool { return m["type"] == "event" && m["topic"] == "ale.state" }
	isEv := func(m map[string]any) bool { return m["type"] == "event" && m["topic"] == "ale.events" }
	if s := w.Next(isState, time.Second); s["data"].(map[string]any)["self"] != "BOB" {
		t.Fatalf("ale.state snapshot = %v", s)
	}
	w.Next(isEv, time.Second)
	w.Next(isEv, time.Second)
	e.Radio.AddAleEvent(rad.AleEvent{Kind: "amd", Peer: "SAM", Text: "HELLO"})
	if ev := w.Next(isEv, 2*time.Second); ev["data"].(map[string]any)["text"] != "HELLO" {
		t.Fatalf("live event = %v", ev)
	}
}
