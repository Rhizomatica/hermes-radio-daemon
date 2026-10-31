package digi_test

import (
	"context"
	"fmt"
	"testing"
	"time"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/digi"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
)

func send(w *testapi.WS, id, args string) map[string]any {
	w.Send(`{"type":"cmd","id":"` + id + `","cmd":"digi.send","args":` + args + `}`)
	return w.Next(func(m map[string]any) bool { return m["type"] == "reply" && m["id"] == id }, 2*time.Second)
}

func errCode(r map[string]any) any {
	if e, ok := r["error"].(map[string]any); ok {
		return e["code"]
	}
	return nil
}

func TestDigiSendModes(t *testing.T) {
	e := testapi.New(t)
	w := e.Open()
	if r := send(w, "1", `{"text":"CQ"}`); errCode(r) != "wrong_mode" {
		t.Fatalf("in USB = %v, want wrong_mode", r)
	}
	_ = e.Radio.SetMode(0, "CW")
	if r := send(w, "2", `{"mode":"RTTY","text":"CQ"}`); errCode(r) != "wrong_mode" {
		t.Fatalf("mode guard = %v, want wrong_mode", r)
	}
	if r := send(w, "3", `{"mode":"PSK","text":"CQ"}`); errCode(r) != "out_of_range" {
		t.Fatalf("unknown mode = %v", r)
	}
	if r := send(w, "4", `{}`); errCode(r) != "missing_arg" {
		t.Fatalf("no text = %v", r)
	}
	if r := send(w, "5", `{"mode":"CW","text":"CQ CQ"}`); r["ok"] != true {
		t.Fatalf("CW = %v", r)
	}
	_ = e.Radio.SetMode(0, "FT8")
	if r := send(w, "6", `{"text":"THIS IS FAR TOO LONG FOR FT8"}`); errCode(r) != "out_of_range" {
		t.Fatalf("long FT8 = %v", r)
	}
	if r := send(w, "7", `{"text":"TNX 73"}`); r["ok"] != true {
		t.Fatalf("FT8 = %v", r)
	}
	if got := e.Radio.DigiSent(); len(got) != 2 || got[0] != "CQ CQ" || got[1] != "TNX 73" {
		t.Fatalf("queued = %q", got)
	}
}

// CW and RTTY identify the station; FT8 and text that already names it don't
// get the suffix.
func TestDigiSendIdentifies(t *testing.T) {
	e := testapi.New(t)
	e.R.StationCallsign = func(context.Context) string { return "pu2uit" }
	w := e.Open()
	_ = e.Radio.SetMode(0, "RTTY")
	if r := send(w, "1", `{"text":"CQ CQ"}`); r["data"].(map[string]any)["text"] != "CQ CQ de PU2UIT" {
		t.Fatalf("RTTY = %v", r)
	}
	if r := send(w, "2", `{"text":"CQ de pu2uit k"}`); r["data"].(map[string]any)["text"] != "CQ de pu2uit k" {
		t.Fatalf("already identified = %v", r)
	}
	_ = e.Radio.SetMode(0, "FT8")
	if r := send(w, "3", `{"text":"TNX 73"}`); r["data"].(map[string]any)["text"] != "TNX 73" {
		t.Fatalf("FT8 = %v", r)
	}
}

func TestDigiSendRateLimit(t *testing.T) {
	e := testapi.New(t)
	w := e.Open()
	_ = e.Radio.SetMode(0, "CW")
	for i := 0; i < digi.DigiPerMinute; i++ {
		if r := send(w, fmt.Sprint(i), `{"text":"CQ"}`); r["ok"] != true {
			t.Fatalf("send %d = %v", i+1, r)
		}
	}
	if r := send(w, "over", `{"text":"CQ"}`); errCode(r) != "rate_limited" {
		t.Fatalf("past the limit = %v", r)
	}
}

// The digi topic: the stored entries as a snapshot, then each new one,
// transmissions included.
func TestDigiTopic(t *testing.T) {
	e := testapi.New(t)
	e.Radio.AddDigi(`{"id":1,"at":"2026-10-31T10:00:00.000Z","mode":"FT8","dir":"rx","text":"CQ PU2UIT GG66"}`)
	w := e.Open()
	w.Send(`{"type":"subscribe","id":"s","topics":["digi"]}`)
	ev := w.Next(func(m map[string]any) bool { return m["type"] == "event" && m["topic"] == "digi" }, time.Second)
	if ev["snapshot"] != true || ev["data"].(map[string]any)["text"] != "CQ PU2UIT GG66" {
		t.Fatalf("snapshot = %v", ev)
	}
	w.Next(func(m map[string]any) bool { return m["type"] == "reply" && m["id"] == "s" }, time.Second)

	_ = e.Radio.SetMode(0, "CW")
	send(w, "1", `{"text":"TEST"}`)
	ev = w.Next(func(m map[string]any) bool { return m["type"] == "event" && m["topic"] == "digi" }, 2*time.Second)
	if ev["snapshot"] == true || ev["data"].(map[string]any)["dir"] != "tx" {
		t.Fatalf("the transmission = %v", ev)
	}
}
