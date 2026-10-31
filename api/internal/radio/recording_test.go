package radio_test

import (
	"testing"
	"time"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
)

func TestRecording(t *testing.T) {
	e := testapi.New(t)
	w := e.Open()
	cmd := func(id, name, args string) map[string]any {
		w.Send(`{"type":"cmd","id":"` + id + `","cmd":"` + name + `","args":` + args + `}`)
		return w.Next(func(m map[string]any) bool { return m["type"] == "reply" && m["id"] == id }, 2*time.Second)
	}
	if r := cmd("1", "recording.start", `{}`); r["ok"] != true || r["data"].(map[string]any)["rx"] != true || r["data"].(map[string]any)["tx"] != true {
		t.Fatalf("start both = %v", r)
	}
	if r := cmd("2", "recording.stop", `{"stream":"tx"}`); r["data"].(map[string]any)["rx"] != true || r["data"].(map[string]any)["tx"] != false {
		t.Fatalf("stop tx = %v", r)
	}
	if r := cmd("3", "recording.stop", `{"stream":"rx"}`); r["data"].(map[string]any)["rx"] != false {
		t.Fatalf("stop rx = %v", r)
	}
	if r := cmd("4", "recording.start", `{"stream":"mic"}`); code(r) != "out_of_range" {
		t.Fatalf("unknown stream = %v", r)
	}
}
