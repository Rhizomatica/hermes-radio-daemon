package radio_test

import (
	"fmt"
	"testing"
	"time"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
)

// ptt sends radio.ptt on w and returns the reply.
func ptt(w *testapi.WS, id string, on bool) map[string]any {
	w.Send(fmt.Sprintf(`{"type":"cmd","id":%q,"cmd":"radio.ptt","args":{"on":%v}}`, id, on))
	return w.Next(func(m map[string]any) bool { return m["type"] == "reply" && m["id"] == id }, 2*time.Second)
}

func code(r map[string]any) any {
	if e, ok := r["error"].(map[string]any); ok {
		return e["code"]
	}
	return nil
}

// The connection that keys owns PTT: another is refused (busy) and cannot
// unkey it (no_ptt); the owner unkeys.
func TestPTTOwnership(t *testing.T) {
	e := testapi.New(t)
	a, b := e.Open(), e.Open()
	if r := ptt(a, "1", true); r["ok"] != true || !e.Radio.State().TX {
		t.Fatalf("key = %v, tx = %v", r, e.Radio.State().TX)
	}
	if r := ptt(b, "2", true); code(r) != "busy" {
		t.Fatalf("second client keying = %v, want busy", r)
	}
	if r := ptt(b, "3", false); code(r) != "no_ptt" || !e.Radio.State().TX {
		t.Fatalf("second client unkeying = %v, tx = %v", r, e.Radio.State().TX)
	}
	if r := ptt(a, "4", true); r["ok"] != true {
		t.Fatalf("the owner keying again = %v", r)
	}
	if r := ptt(a, "5", false); r["ok"] != true || e.Radio.State().TX {
		t.Fatalf("unkey = %v, tx = %v", r, e.Radio.State().TX)
	}
	if r := ptt(a, "6", false); code(r) != "no_ptt" {
		t.Fatalf("unkey when not keyed = %v", r)
	}
	// radiod's close unkeys the connection that held PTT.
	ptt(b, "7", true)
	e.Radio.ReleasePTT(b.ID)
	if r := ptt(a, "8", true); r["ok"] != true {
		t.Fatalf("keying after the holder left = %v", r)
	}
}

func TestPTTRefusals(t *testing.T) {
	e := testapi.New(t)
	w := e.Open()
	if r := ptt(w, "a", true); r["ok"] != true {
		t.Fatal(r)
	}
	ptt(w, "b", false)

	e.Radio.SetProtection(true)
	if r := ptt(w, "c", true); code(r) != "busy" || e.Radio.State().TX {
		t.Fatalf("with the SWR protection tripped = %v", r)
	}
	e.Radio.SetProtection(false)

	if err := e.Radio.SetMode(0, "DSTAR"); err != nil {
		t.Fatal(err)
	}
	if err := e.Radio.SetDigi("dstar_encrypt", 1); err != nil {
		t.Fatal(err)
	}
	// Encrypted voice keys like any other: encryption is the station's setting.
	if r := ptt(w, "d", true); r["ok"] != true || !e.Radio.State().TX {
		t.Fatalf("encrypted D-STAR = %v", r)
	}

	w.Send(`{"type":"cmd","id":"g","cmd":"radio.ptt","args":{}}`)
	if r := w.Next(func(m map[string]any) bool { return m["id"] == "g" }, time.Second); code(r) != "missing_arg" {
		t.Fatalf("no on = %v", r)
	}
}

// One user keys at most PTTPerMinute times a minute.
func TestPTTRateLimit(t *testing.T) {
	e := testapi.New(t)
	w := e.Open()
	for i := 0; i < radio.PTTPerMinute; i++ {
		if r := ptt(w, fmt.Sprint("on", i), true); r["ok"] != true {
			t.Fatalf("key %d = %v", i+1, r)
		}
		ptt(w, fmt.Sprint("off", i), false)
	}
	if r := ptt(w, "over", true); code(r) != "rate_limited" || e.Radio.State().TX {
		t.Fatalf("key past the limit = %v", r)
	}
	// Unkeying is never limited.
	if r := ptt(w, "off-again", false); code(r) != "no_ptt" {
		t.Fatalf("unkey = %v", r)
	}
}
