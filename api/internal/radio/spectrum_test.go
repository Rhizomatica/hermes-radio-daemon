package radio_test

import (
	"testing"
	"time"

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

	x := e.Open()
	x.Send(`{"type":"subscribe","id":"b","topics":["radio.spectrum.rx","no.such.topic"]}`)
	x.Next(func(m map[string]any) bool { return m["type"] == "reply" && m["id"] == "b" }, time.Second)
	if e.Radio.Spectrum(x.ID, false) {
		t.Fatal("a failed subscribe left the C subscription")
	}
}
