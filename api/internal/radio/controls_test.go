package radio_test

import (
	"encoding/json"
	"strings"
	"testing"
	"time"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
)

func ctrlErrCode(r map[string]any) string {
	e, _ := r["error"].(map[string]any)
	c, _ := e["code"].(string)
	return c
}

func replyData(r map[string]any) map[string]any { d, _ := r["data"].(map[string]any); return d }

// rig.get reads the named controls into levels, funcs and parms; names the
// rig does not have, or cannot read, are refused before anything is read.
func TestRigGet(t *testing.T) {
	e := testapi.New(t)
	r := e.Cmd("rig.get", `{"names":["AF","NB","BACKLIGHT","PREAMP"]}`)
	d := replyData(r)
	if r["ok"] != true || d["levels"].(map[string]any)["AF"] != 0.5 || d["levels"].(map[string]any)["PREAMP"] != 0.0 ||
		d["funcs"].(map[string]any)["NB"] != false || d["parms"].(map[string]any)["BACKLIGHT"] != 0.8 {
		t.Fatalf("rig.get = %v", r)
	}
	reads := e.Radio.ControlReads()
	for args, code := range map[string]string{
		`{"names":["AF","NOSUCH"]}`: "not_supported",
		`{"names":["TUNER"]}`:       "not_supported", // settable only
		`{"names":[]}`:              "missing_arg",
		`{}`:                        "missing_arg",
		`{"names":["AF","AF"]}`:     "bad_request",
		`{"names":["AF"],"x":1}`:    "bad_request",
	} {
		if r := e.Cmd("rig.get", args); r["ok"] != false || ctrlErrCode(r) != code {
			t.Errorf("rig.get %s = %v, want %s", args, r, code)
		}
	}
	if n := e.Radio.ControlReads(); n != reads {
		t.Errorf("refused rig.get calls read the rig %d times", n-reads)
	}
	e.Radio.RigDown(true)
	if r := e.Cmd("rig.get", `{"names":["AF"]}`); ctrlErrCode(r) != "rig_timeout" {
		t.Errorf("rig.get with the rig not answering = %v", r)
	}
}

// rig.set: a function takes true/false, a level a number within its range.
func TestRigSet(t *testing.T) {
	e := testapi.New(t)
	if r := e.Cmd("rig.set", `{"name":"AF","value":0.7}`); r["ok"] != true || replyData(r)["levels"].(map[string]any)["AF"] != 0.7 {
		t.Fatalf("rig.set AF = %v", r)
	}
	if r := e.Cmd("rig.set", `{"name":"NB","value":true}`); r["ok"] != true || replyData(r)["funcs"].(map[string]any)["NB"] != true {
		t.Fatalf("rig.set NB = %v", r)
	}
	if r := e.Cmd("rig.get", `{"names":["AF","NB"]}`); replyData(r)["levels"].(map[string]any)["AF"] != 0.7 ||
		replyData(r)["funcs"].(map[string]any)["NB"] != true {
		t.Errorf("read back = %v", r)
	}
	for args, code := range map[string]string{
		`{"name":"AF","value":2}`:       "out_of_range",
		`{"name":"AF","value":true}`:    "bad_request",
		`{"name":"NB","value":1}`:       "bad_request",
		`{"name":"STRENGTH","value":0}`: "not_supported", // a meter: read only
		`{"name":"NOSUCH","value":1}`:   "not_supported",
		`{"name":"AF"}`:                 "missing_arg",
		`{"value":1}`:                   "missing_arg",
	} {
		if r := e.Cmd("rig.set", args); r["ok"] != false || ctrlErrCode(r) != code {
			t.Errorf("rig.set %s = %v, want %s", args, r, code)
		}
	}
	if r := e.Cmd("rig.get", `{"names":["AF"]}`); replyData(r)["levels"].(map[string]any)["AF"] != 0.7 {
		t.Errorf("a refused rig.set changed AF: %v", r)
	}
}

func rigEvent(m map[string]any) bool { return m["type"] == "event" && m["topic"] == "radio.rig" }

// rig.watch: radio.rig events carry the watched controls, read every
// interval, and only what changed; an empty list stops the reads.
func TestRigWatch(t *testing.T) {
	e := testapi.New(t)
	w := e.Open()
	if h := w.Next(testapi.Type("hello"), time.Second); !strings.Contains(strings.Join(strs(h["topics"]), ","), "radio.rig") {
		t.Fatalf("hello topics = %v", h["topics"])
	}
	w.Send(`{"type":"subscribe","id":"s","topics":["radio.rig"]}`)
	snap := w.Next(rigEvent, time.Second)
	if b, _ := json.Marshal(snap["data"]); string(b) != `{"funcs":{},"levels":{},"parms":{}}` {
		t.Fatalf("snapshot without a watch = %s", b)
	}
	w.Next(testapi.Type("reply"), time.Second) // the subscribe's, after the snapshot

	w.Send(`{"type":"cmd","id":"w","cmd":"rig.watch","args":{"names":["NB","AF"],"interval_ms":250}}`)
	r := w.Next(testapi.Type("reply"), time.Second)
	if r["ok"] != true || replyData(r)["interval_ms"] != 250.0 || strings.Join(strs(replyData(r)["names"]), ",") != "AF,NB" {
		t.Fatalf("rig.watch = %v", r)
	}
	ev := w.Next(rigEvent, 2*time.Second)
	if d := ev["data"].(map[string]any); d["levels"].(map[string]any)["AF"] != 0.5 || d["funcs"].(map[string]any)["NB"] != false {
		t.Fatalf("first watched values = %v", ev)
	}

	e.Radio.SetControlValue("AF", 0.9) // the front panel
	ev = w.Next(rigEvent, 2*time.Second)
	if b, _ := json.Marshal(ev["data"]); string(b) != `{"levels":{"AF":0.9}}` {
		t.Errorf("change event = %s, want only levels.AF", b)
	}

	w.Send(`{"type":"cmd","id":"x","cmd":"rig.watch","args":{"names":[]}}`)
	w.Next(testapi.Type("reply"), time.Second)
	time.Sleep(100 * time.Millisecond)
	before := e.Radio.ControlReads()
	time.Sleep(700 * time.Millisecond)
	if n := e.Radio.ControlReads(); n != before {
		t.Errorf("the rig was read %d more times after the watch stopped", n-before)
	}
}

// A watch stops reading the rig when its connection closes.
func TestRigWatchStopsOnClose(t *testing.T) {
	e := testapi.New(t)
	w := e.Open()
	w.Send(`{"type":"cmd","id":"w","cmd":"rig.watch","args":{"names":["AF"],"interval_ms":250}}`)
	w.Next(testapi.Type("reply"), time.Second)
	time.Sleep(600 * time.Millisecond)
	if e.Radio.ControlReads() < 2 {
		t.Fatalf("the watch read the rig %d times in 600 ms", e.Radio.ControlReads())
	}
	e.R.WSClose(w.ID)
	time.Sleep(100 * time.Millisecond)
	before := e.Radio.ControlReads()
	time.Sleep(700 * time.Millisecond)
	if n := e.Radio.ControlReads(); n != before {
		t.Errorf("the rig was read %d more times after the connection closed", n-before)
	}
}

func TestRigWatchRefusals(t *testing.T) {
	e := testapi.New(t)
	for args, code := range map[string]string{
		`{"names":["AF"],"interval_ms":100}`:    "out_of_range",
		`{"names":["AF"],"interval_ms":120000}`: "out_of_range",
		`{"names":["NOSUCH"]}`:                  "not_supported",
		`{}`:                                    "missing_arg",
	} {
		if r := e.Cmd("rig.watch", args); r["ok"] != false || ctrlErrCode(r) != code {
			t.Errorf("rig.watch %s = %v, want %s", args, r, code)
		}
	}
}

func strs(v any) []string {
	l, _ := v.([]any)
	out := make([]string, 0, len(l))
	for _, x := range l {
		s, _ := x.(string)
		out = append(out, s)
	}
	return out
}
