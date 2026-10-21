package api_test

import (
	"bytes"
	"errors"
	"fmt"
	"net/http"
	"sync"
	"testing"
	"time"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
	"github.com/Rhizomatica/hermes-radio-daemon/api/spec"
)

// state is a topic's state the test changes.
type state struct {
	mu sync.Mutex
	m  map[string]any
}

func (s *state) set(k string, v any) { s.mu.Lock(); s.m[k] = v; s.mu.Unlock() }

func (s *state) get() (any, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	out := map[string]any{}
	for k, v := range s.m {
		out[k] = v
	}
	return out, nil
}

// env is a router serving one topic, radio.state, over st.
func env(t *testing.T, role spec.Role) (*testapi.Env, *state) {
	stub := rad.NewStub()
	rt := api.NewRouter(stub)
	st := &state{m: map[string]any{"frequency_hz": 7100000, "mode": "USB"}}
	rt.Topic("radio.state", role, st.get)
	return &testapi.Env{T: t, Radio: stub, R: rt}, st
}

func event(topic string) func(map[string]any) bool {
	return func(m map[string]any) bool { return m["type"] == "event" && m["topic"] == topic }
}

// Subscribing gives the whole state (seq 0, snapshot); a change gives the
// changed fields only, with the next seq; no change, no event.
func TestTopicSnapshotThenChanges(t *testing.T) {
	e, st := env(t, spec.RoleReadonly)
	w := e.Open()

	hello := w.Next(testapi.Type("hello"), time.Second)
	if topics, _ := hello["topics"].([]any); len(topics) != 1 || topics[0] != "radio.state" {
		t.Errorf("hello topics = %v, want [radio.state]", hello["topics"])
	}
	if lim, _ := hello["limits"].(map[string]any); lim["max_events_per_s"] != 10.0 {
		t.Errorf("hello limits = %v, want max_events_per_s 10", hello["limits"])
	}

	w.Send(`{"type":"subscribe","id":"s1","topics":["radio.state"]}`)
	snap := w.Next(event("radio.state"), time.Second)
	data, _ := snap["data"].(map[string]any)
	if snap["seq"] != 0.0 || snap["snapshot"] != true || data["frequency_hz"] != 7100000.0 || data["mode"] != "USB" || snap["ts"] == nil {
		t.Fatalf("snapshot = %v", snap)
	}
	if r := w.Next(testapi.Type("reply"), time.Second); r["id"] != "s1" || r["ok"] != true {
		t.Fatalf("subscribe reply = %v", r)
	}

	st.set("frequency_hz", 7074000)
	ev := w.Next(event("radio.state"), time.Second)
	data, _ = ev["data"].(map[string]any)
	if ev["seq"] != 1.0 || ev["snapshot"] != nil || data["frequency_hz"] != 7074000.0 || len(data) != 1 {
		t.Fatalf("change event = %v, want seq 1 with frequency_hz only", ev)
	}
	w.None(event("radio.state"), 300*time.Millisecond)

	st.set("mode", "LSB")
	if ev := w.Next(event("radio.state"), time.Second); ev["seq"] != 2.0 {
		t.Fatalf("second change = %v, want seq 2", ev)
	}
}

// However often the state changes, a topic sends at most 10 events a second.
func TestTopicRateLimit(t *testing.T) {
	e, st := env(t, spec.RoleReadonly)
	w := e.Open()
	w.Send(`{"type":"subscribe","id":"s1","topics":["radio.state"]}`)
	w.Next(event("radio.state"), time.Second)

	stop := time.Now().Add(time.Second)
	for i := 0; time.Now().Before(stop); i++ {
		st.set("frequency_hz", 7000000+i)
		time.Sleep(2 * time.Millisecond)
	}
	time.Sleep(150 * time.Millisecond)
	n := 0
	for _, f := range e.Radio.Frames(w.ID) {
		if bytes.Contains(f, []byte(`"type":"event"`)) {
			n++
		}
	}
	if n-1 > 12 { // the snapshot, then about 10 in the second
		t.Errorf("%d events in about 1.15 s of constant change, want at most about 10 a second", n-1)
	}
	if n-1 < 5 {
		t.Errorf("only %d events in about 1.15 s of constant change", n-1)
	}
}

// The reply to a subscribe comes after every topic's snapshot.
func TestTopicReplyAfterSnapshot(t *testing.T) {
	e, _ := env(t, spec.RoleReadonly)
	w := e.Open()
	w.Next(testapi.Type("hello"), time.Second)
	w.Send(`{"type":"subscribe","id":"s1","topics":["radio.state"]}`)
	w.Next(testapi.Type("reply"), time.Second)
	var order []string
	for _, f := range e.Radio.Frames(w.ID) {
		switch {
		case bytes.Contains(f, []byte(`"snapshot":true`)):
			order = append(order, "snapshot")
		case bytes.Contains(f, []byte(`"type":"reply"`)):
			order = append(order, "reply")
		}
	}
	if len(order) != 2 || order[0] != "snapshot" || order[1] != "reply" {
		t.Fatalf("frames = %v, want [snapshot reply]", order)
	}
}

// After unsubscribing, no more events; subscribing again starts at seq 0.
func TestTopicUnsubscribeResubscribe(t *testing.T) {
	e, st := env(t, spec.RoleReadonly)
	w := e.Open()
	w.Send(`{"type":"subscribe","id":"s1","topics":["radio.state"]}`)
	w.Next(event("radio.state"), time.Second)
	st.set("frequency_hz", 7001000)
	w.Next(event("radio.state"), time.Second)

	w.Send(`{"type":"unsubscribe","id":"u1","topics":["radio.state"]}`)
	if r := w.Next(testapi.Type("reply"), time.Second); r["id"] != "u1" || r["ok"] != true {
		t.Fatalf("unsubscribe reply = %v", r)
	}
	st.set("frequency_hz", 7002000)
	w.None(event("radio.state"), 300*time.Millisecond)

	w.Send(`{"type":"subscribe","id":"s2","topics":["radio.state"]}`)
	snap := w.Next(event("radio.state"), time.Second)
	if snap["seq"] != 0.0 || snap["snapshot"] != true {
		t.Fatalf("snapshot after resubscribing = %v", snap)
	}
}

// Every topic of a subscribe is checked before any is subscribed.
func TestTopicSubscribeErrors(t *testing.T) {
	e, _ := env(t, spec.RoleReadonly)
	w := e.Open()
	cases := []struct{ frame, code string }{
		{`{"type":"subscribe","id":"a","topics":["radio.nonsense"]}`, "bad_request"},
		{`{"type":"subscribe","id":"b","topics":["radio.state","gps"]}`, "not_supported"},
		{`{"type":"subscribe","id":"c","topics":[]}`, "missing_arg"},
		{`{"type":"subscribe","id":"d"}`, "missing_arg"},
	}
	for _, c := range cases {
		w.Send(c.frame)
		r := w.Next(testapi.Type("reply"), time.Second)
		er, _ := r["error"].(map[string]any)
		if r["ok"] != false || er["code"] != c.code {
			t.Errorf("%s: reply %v, want error %s", c.frame, r, c.code)
		}
	}
	w.None(event("radio.state"), 200*time.Millisecond) // "b" subscribed nothing
}

// A topic above the connection's role is refused, and not listed in hello.
func TestTopicRole(t *testing.T) {
	e, _ := env(t, spec.RoleOperator)
	e.R.RoleOf = func(*http.Request) spec.Role { return spec.RoleReadonly }
	w := e.Open()
	if hello := w.Next(testapi.Type("hello"), time.Second); len(hello["topics"].([]any)) != 0 {
		t.Errorf("hello offers %v to readonly", hello["topics"])
	}
	w.Send(`{"type":"subscribe","id":"s1","topics":["radio.state"]}`)
	r := w.Next(testapi.Type("reply"), time.Second)
	if er, _ := r["error"].(map[string]any); r["ok"] != false || er["code"] != "forbidden" {
		t.Fatalf("readonly subscribing to an operator topic: %v", r)
	}
}

func TestTopicRegistration(t *testing.T) {
	rt := api.NewRouter(rad.NewStub())
	none := func() (any, error) { return map[string]any{}, nil }
	mustPanic := func(what string, f func()) {
		defer func() {
			if recover() == nil {
				t.Errorf("%s did not panic", what)
			}
		}()
		f()
	}
	mustPanic("a topic the spec does not have", func() { rt.Topic("radio.nonsense", spec.RoleReadonly, none) })
	rt.Topic("radio.state", spec.RoleReadonly, none)
	mustPanic("a second registration", func() { rt.Topic("radio.state", spec.RoleReadonly, none) })
}

// A topic without state (its source is not running) is not offered, and a
// subscribe naming it subscribes nothing; once it has state it is served.
func TestTopicWithoutState(t *testing.T) {
	stub := rad.NewStub()
	rt := api.NewRouter(stub)
	var mu sync.Mutex
	up := false
	rt.Topic("radio.state", spec.RoleReadonly, func() (any, error) { return map[string]any{"a": 1}, nil })
	rt.Topic("modem.status", spec.RoleReadonly, func() (any, error) {
		mu.Lock()
		defer mu.Unlock()
		if !up {
			return nil, errors.New("not running")
		}
		return map[string]any{"link": "idle"}, nil
	})
	e := &testapi.Env{T: t, Radio: stub, R: rt}

	w := e.Open()
	if h := w.Next(testapi.Type("hello"), time.Second); fmt.Sprint(h["topics"]) != "[radio.state]" {
		t.Errorf("hello topics = %v, want [radio.state]", h["topics"])
	}
	w.Send(`{"type":"subscribe","id":"s","topics":["radio.state","modem.status"]}`)
	r := w.Next(testapi.Type("reply"), time.Second)
	if er, _ := r["error"].(map[string]any); r["ok"] != false || er["code"] != "not_supported" {
		t.Fatalf("subscribe = %v, want not_supported", r)
	}
	w.None(event("radio.state"), 200*time.Millisecond) // all or nothing

	mu.Lock()
	up = true
	mu.Unlock()
	w.Send(`{"type":"subscribe","id":"s2","topics":["modem.status"]}`)
	if snap := w.Next(event("modem.status"), time.Second); snap["snapshot"] != true {
		t.Errorf("snapshot once running = %v", snap)
	}
}
