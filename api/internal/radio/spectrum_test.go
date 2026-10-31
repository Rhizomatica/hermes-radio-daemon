package radio_test

import (
	"testing"
	"time"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
)

// Subscribing to a spectrum tells C (which sends the frames) and sends no
// JSON snapshot; unsubscribing tells C to stop; a subscription that fails
// on a later topic undoes the C one.
func TestSpectrumTopics(t *testing.T) {
	e := testapi.New(t)
	w := e.Open()
	reply := func(id string) map[string]any {
		return w.Next(func(m map[string]any) bool { return m["type"] == "reply" && m["id"] == id }, time.Second)
	}
	w.Send(`{"type":"subscribe","id":"s","topics":["radio.spectrum.rx","radio.spectrum.tx"]}`)
	if r := reply("s"); r["ok"] != true || !e.Radio.Spectrum(w.ID, false) || !e.Radio.Spectrum(w.ID, true) {
		t.Fatalf("subscribe = %v", r)
	}
	w.None(func(m map[string]any) bool { return m["type"] == "event" }, 300*time.Millisecond)

	w.Send(`{"type":"unsubscribe","id":"u","topics":["radio.spectrum.tx"]}`)
	if reply("u"); !e.Radio.Spectrum(w.ID, false) || e.Radio.Spectrum(w.ID, true) {
		t.Fatal("unsubscribe did not reach C")
	}

	// A subscribe that fails leaves what the connection already held.
	w.Send(`{"type":"subscribe","id":"r","topics":["radio.spectrum.rx","no.such.topic"]}`)
	if r := reply("r"); r["ok"] != false || !e.Radio.Spectrum(w.ID, false) {
		t.Fatalf("a failed re-subscribe = %v, rx still subscribed = %v", r, e.Radio.Spectrum(w.ID, false))
	}

	x := e.Open()
	x.Send(`{"type":"subscribe","id":"b","topics":["radio.spectrum.rx","no.such.topic"]}`)
	x.Next(func(m map[string]any) bool { return m["type"] == "reply" && m["id"] == "b" }, time.Second)
	if e.Radio.Spectrum(x.ID, false) {
		t.Fatal("a failed subscribe left the C subscription")
	}

	// A connection that closes drops its C subscriptions, even one taken
	// after C's own close dropped them.
	w.Close()
	deadline := time.Now().Add(time.Second)
	for e.Radio.Spectrum(w.ID, false) {
		if time.Now().After(deadline) {
			t.Fatal("a closed connection kept its spectrum subscription")
		}
		time.Sleep(time.Millisecond)
	}
}

// One user holds at most api.BinaryPerUser connections on a spectrum, so
// they cannot take every one of C's station-wide slots.
func TestSpectrumPerUser(t *testing.T) {
	e := testapi.New(t)
	var ws []*testapi.WS
	sub := func(w *testapi.WS, id string) map[string]any {
		w.Send(`{"type":"subscribe","id":"` + id + `","topics":["radio.spectrum.rx"]}`)
		return w.Next(func(m map[string]any) bool { return m["type"] == "reply" && m["id"] == id }, time.Second)
	}
	for i := range api.BinaryPerUser {
		w := e.Open()
		if r := sub(w, "s"); r["ok"] != true {
			t.Fatalf("connection %d: %v", i, r)
		}
		ws = append(ws, w)
	}
	over := e.Open()
	if r := sub(over, "s"); code(r) != "busy" || e.Radio.Spectrum(over.ID, false) {
		t.Fatalf("one connection over the cap = %v", r)
	}
	ws[0].Close()
	if r := sub(over, "t"); r["ok"] != true {
		t.Fatalf("after one closed = %v", r)
	}
}
