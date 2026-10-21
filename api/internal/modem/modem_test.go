package modem_test

import (
	"fmt"
	"reflect"
	"strconv"
	"strings"
	"testing"
	"time"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
)

func TestModemStatus(t *testing.T) {
	e := testapi.New(t)
	if r := e.Do("GET", "/modem", ""); r.Status != 503 || r.JSON(t)["code"] != "not_running" {
		t.Fatalf("without mercury = %d %s", r.Status, r.Body)
	}
	e.Radio.SetModem(`{"type":"status","bitrate":2400,"snr":7.5,"user_callsign":"PU2UIT","dest_callsign":"PY2XYZ",` +
		`"sync":true,"direction":"rx","bytes_transmitted":10,"bytes_received":20,"tx_gain_db":3,"tx_peak_dbfs":-6.5,` +
		`"audio_ok":true,"audio_error":"","arq_tx_mode":"DATAC1","arq_rx_mode":"","peer_snr":-99.9,"peer_snr_valid":false}`)
	j := e.Do("GET", "/modem", "").JSON(t)
	want := map[string]any{"link": "connected", "my_call": "PU2UIT", "peer_call": "PY2XYZ", "direction": "rx",
		"snr_db": 7.5, "bitrate_bps": 2400.0, "bytes_tx": 10.0, "bytes_rx": 20.0, "arq_tx_mode": "DATAC1",
		"arq_rx_mode": nil, "peer_snr_db": nil, "audio_ok": true, "audio_error": nil}
	for k, v := range want {
		if j[k] != v {
			t.Errorf("%s = %v, want %v", k, j[k], v)
		}
	}
	if e.Do("GET", "/health", "").JSON(t)["components"].(map[string]any)["modem"] != "ok" {
		t.Error("health does not report the modem")
	}
}

func mercuryStatus(bytesRx int) string {
	return `{"type":"status","bitrate":980,"snr":12.5,"user_callsign":"N0CALL","dest_callsign":"N1CALL",` +
		`"sync":true,"direction":"rx","bytes_transmitted":0,"bytes_received":` + strconv.Itoa(bytesRx) +
		`,"tx_gain_db":0,"tx_peak_dbfs":-6,"audio_ok":true,"audio_error":"","arq_tx_mode":"","arq_rx_mode":"DATAC1",` +
		`"peer_snr":-99.9,"peer_snr_valid":false}`
}

func modemStatus(m map[string]any) bool { return m["type"] == "event" && m["topic"] == "modem.status" }

// modem.status: offered only while mercury runs; its snapshot is GET /modem,
// and an event carries what changed in mercury's status.
func TestModemStatusTopic(t *testing.T) {
	e := testapi.New(t)
	w := e.Open()
	if h := w.Next(testapi.Type("hello"), time.Second); strings.Contains(fmt.Sprint(h["topics"]), "modem.status") {
		t.Errorf("hello offers modem.status without mercury: %v", h["topics"])
	}
	w.Send(`{"type":"subscribe","id":"s","topics":["modem.status"]}`)
	r := w.Next(testapi.Type("reply"), time.Second)
	if er, _ := r["error"].(map[string]any); r["ok"] != false || er["code"] != "not_supported" {
		t.Fatalf("subscribe without mercury = %v", r)
	}

	e.Radio.SetModem(mercuryStatus(100))
	w = e.Open()
	if h := w.Next(testapi.Type("hello"), time.Second); !strings.Contains(fmt.Sprint(h["topics"]), "modem.status") {
		t.Errorf("hello does not offer modem.status with mercury running: %v", h["topics"])
	}
	w.Send(`{"type":"subscribe","id":"s","topics":["modem.status"]}`)
	snap := w.Next(modemStatus, time.Second)
	rest := e.Do("GET", "/modem", "").JSON(t)
	data := snap["data"].(map[string]any)
	for k, v := range rest {
		if !reflect.DeepEqual(data[k], v) {
			t.Errorf("snapshot %s = %v, GET /modem says %v", k, data[k], v)
		}
	}
	if data["link"] != "connected" || data["peer_call"] != "N1CALL" {
		t.Errorf("snapshot = %v", data)
	}

	e.Radio.SetModem(mercuryStatus(2048))
	ev := w.Next(modemStatus, time.Second)
	if d := ev["data"].(map[string]any); d["bytes_rx"] != 2048.0 || len(d) != 1 || ev["seq"] != 1.0 {
		t.Errorf("event = %v, want seq 1 with bytes_rx only", ev)
	}

	e.Radio.SetModem("") // mercury stops: no events, and no bogus "everything changed"
	w.None(modemStatus, 300*time.Millisecond)
}

// mercury's status with the listen mode and the carousel (mercury_host.h).
func statusWith(listen, bcast string) string {
	return `{"type":"status","bitrate":0,"snr":0,"user_callsign":"N0CALL","dest_callsign":"","sync":false,` +
		`"direction":"rx","bytes_transmitted":0,"bytes_received":0,"tx_gain_db":2.5,"tx_peak_dbfs":-6,` +
		`"waterfall":true,"audio_ok":true,"audio_error":"","arq_tx_mode":"","arq_rx_mode":"",` +
		`"peer_snr":0,"peer_snr_valid":false,"listen_mode":1,"listen_mode_name":"` + listen + `","busy":"",` +
		`"broadcast":` + bcast + `}`
}

func TestModemStatusBroadcast(t *testing.T) {
	e := testapi.New(t)
	e.Radio.SetModem(statusWith("DATAC3", `{"state":"idle","cycle":0,"cycles_total":0,"frames_sent":0}`))
	j := e.Do("GET", "/modem", "").JSON(t)
	if j["listen_mode"] != "DATAC3" {
		t.Errorf("listen_mode = %v", j["listen_mode"])
	}
	idle := map[string]any{"state": "idle", "message_id": nil, "mode": nil, "cycle": nil,
		"cycles_total": nil, "frames_sent": nil, "frames_total": nil}
	if !reflect.DeepEqual(j["broadcast"], idle) {
		t.Errorf("broadcast = %v, want %v", j["broadcast"], idle)
	}

	e.Radio.SetModem(statusWith("DATAC1", `{"state":"sending","cycle":2,"cycles_total":4,"frames_sent":17}`))
	b := e.Do("GET", "/modem", "").JSON(t)["broadcast"].(map[string]any)
	if b["state"] != "sending" || b["mode"] != "DATAC1" || b["cycle"] != 2.0 || b["cycles_total"] != 4.0 || b["frames_sent"] != 17.0 {
		t.Errorf("sending = %v", b)
	}
	e.Radio.SetModem(statusWith("DATAC1", `{"state":"stopped_by_arq","cycle":1,"cycles_total":0,"frames_sent":3}`))
	b = e.Do("GET", "/modem", "").JSON(t)["broadcast"].(map[string]any)
	if b["state"] != "preempted" || b["cycles_total"] != nil {
		t.Errorf("stopped by ARQ = %v, want preempted, cycles_total null (until stopped)", b)
	}
	for _, st := range []string{"done", "stopped", "error"} {
		e.Radio.SetModem(statusWith("DATAC1", `{"state":"`+st+`","cycle":4,"cycles_total":4,"frames_sent":40}`))
		if b := e.Do("GET", "/modem", "").JSON(t)["broadcast"].(map[string]any); b["state"] != "idle" {
			t.Errorf("%s = %v, want idle", st, b)
		}
	}
	// a mode the API has no name for (not switchable at run time): null
	e.Radio.SetModem(statusWith("DATAC0", `{"state":"idle","cycle":0,"cycles_total":0,"frames_sent":0}`))
	if j := e.Do("GET", "/modem", "").JSON(t); j["listen_mode"] != nil {
		t.Errorf("DATAC0 listen_mode = %v, want null", j["listen_mode"])
	}
}

func TestModemConfig(t *testing.T) {
	e := testapi.New(t)
	if r := e.Do("GET", "/modem/config", ""); r.Status != 503 {
		t.Fatalf("without mercury = %d %s", r.Status, r.Body)
	}
	if r := e.Do("PATCH", "/modem/config", `{"listen_mode":"DATAC1"}`); r.Status != 503 {
		t.Fatalf("PATCH without mercury = %d %s", r.Status, r.Body)
	}
	e.Radio.SetModem(statusWith("DATAC3", `{"state":"idle","cycle":0,"cycles_total":0,"frames_sent":0}`))
	j := e.Do("GET", "/modem/config", "").JSON(t)
	if j["listen_mode"] != "DATAC3" || j["tx_gain_db"] != 2.5 || j["waterfall"] != true {
		t.Errorf("config = %v", j)
	}

	// every field checked before any applied
	r := e.Do("PATCH", "/modem/config", `{"listen_mode":"DATAC0","tx_gain_db":30}`)
	if r.Status != 422 {
		t.Fatalf("bad patch = %d %s", r.Status, r.Body)
	}
	if errs := r.JSON(t)["errors"].(map[string]any); errs["listen_mode"] == nil || errs["tx_gain_db"] == nil {
		t.Errorf("errors = %v", errs)
	}
	if m, _ := e.Radio.ListenMode(); m != "DATAC3" {
		t.Errorf("a refused patch changed the listen mode to %s", m)
	}

	r = e.Do("PATCH", "/modem/config", `{"listen_mode":"DATAC1","tx_gain_db":-3,"waterfall":false}`)
	if r.Status != 200 {
		t.Fatalf("patch = %d %s", r.Status, r.Body)
	}
	if j := r.JSON(t); j["listen_mode"] != "DATAC1" || j["tx_gain_db"] != -3.0 || j["waterfall"] != false {
		t.Errorf("reply = %v", j)
	}
	if m, _ := e.Radio.ListenMode(); m != "DATAC1" {
		t.Errorf("listen mode = %s", m)
	}
	if g, wf := e.Radio.ModemSettings(); g != -3 || wf {
		t.Errorf("gain %v waterfall %v", g, wf)
	}

	// busy: 409 saying why, and nothing applied
	for why, detail := range map[string]string{"arq": "ARQ", "broadcast": "broadcast"} {
		e.Radio.SetModemBusy(why)
		r = e.Do("PATCH", "/modem/config", `{"listen_mode":"DATAC4","tx_gain_db":5}`)
		if j := r.JSON(t); r.Status != 409 || j["code"] != "busy" || !strings.Contains(fmt.Sprint(j["detail"]), detail) {
			t.Errorf("busy %s = %d %v", why, r.Status, j)
		}
		if g, _ := e.Radio.ModemSettings(); g != -3 {
			t.Errorf("busy %s: the gain changed to %v", why, g)
		}
	}
}

func chatEvent(m map[string]any) bool { return m["type"] == "event" && m["topic"] == "modem.chat" }

// modem.chat: the stored lines as snapshot events, then each new one; and
// modem.chat.send's line comes back on it.
func TestModemChat(t *testing.T) {
	e := testapi.New(t)
	e.Radio.SetModem(statusWith("DATAC3", `{"state":"idle","cycle":0,"cycles_total":0,"frames_sent":0}`))
	e.Radio.AddChat("bcast", "PY2XYZ", "hello all")
	e.Radio.AddChat("arq", "PY2XYZ", "hi")

	w := e.Open()
	if h := w.Next(testapi.Type("hello"), time.Second); !strings.Contains(fmt.Sprint(h["topics"]), "modem.chat") {
		t.Errorf("hello does not offer modem.chat: %v", h["topics"])
	}
	w.Send(`{"type":"subscribe","id":"s","topics":["modem.chat"]}`)
	if r := w.Next(testapi.Type("reply"), time.Second); r["ok"] != true {
		t.Fatalf("subscribe = %v", r)
	}
	for i, want := range []map[string]any{
		{"plane": "broadcast", "dir": "rx", "peer": "PY2XYZ", "text": "hello all"},
		{"plane": "arq", "dir": "rx", "peer": "PY2XYZ", "text": "hi"},
	} {
		ev := w.Next(chatEvent, time.Second)
		d := ev["data"].(map[string]any)
		if ev["snapshot"] != true || ev["seq"] != float64(i) {
			t.Errorf("history %d: snapshot %v seq %v", i, ev["snapshot"], ev["seq"])
		}
		for k, v := range want {
			if d[k] != v {
				t.Errorf("history %d: %s = %v, want %v", i, k, d[k], v)
			}
		}
		if _, err := time.Parse(time.RFC3339, fmt.Sprint(d["at"])); err != nil {
			t.Errorf("history %d: at = %v", i, d["at"])
		}
	}

	if r := e.Cmd("modem.chat.send", `{"plane":"broadcast","text":"CQ"}`); r["ok"] != true {
		t.Fatalf("send = %v", r)
	}
	ev := w.Next(chatEvent, time.Second)
	if d := ev["data"].(map[string]any); ev["snapshot"] == true || ev["seq"] != 2.0 ||
		d["dir"] != "tx" || d["text"] != "CQ" || d["plane"] != "broadcast" || d["peer"] != nil {
		t.Errorf("sent line = %v", ev)
	}
	w.None(chatEvent, 300*time.Millisecond) // once

	for _, c := range []struct{ args, code string }{
		{`{"plane":"arq","text":"x"}`, "bad_request"}, // no session up
		{`{"plane":"broadcast","text":"` + strings.Repeat("x", 200) + `"}`, "out_of_range"},
		{`{"plane":"vhf","text":"x"}`, "out_of_range"},
		{`{"plane":"arq"}`, "missing_arg"},
		{`{"plane":"arq","text":""}`, "out_of_range"},
	} {
		r := e.Cmd("modem.chat.send", c.args)
		if er, _ := r["error"].(map[string]any); r["ok"] != false || er["code"] != c.code {
			t.Errorf("send %s = %v, want %s", c.args, r, c.code)
		}
	}
	e.Radio.SetModemBusy("arq")
	if r := e.Cmd("modem.chat.send", `{"plane":"arq","text":"over"}`); r["ok"] != true {
		t.Errorf("send on a session = %v", r)
	}
}

// The snapshot is the last chatSnapshot lines; a new subscriber after them
// gets only what follows.
func TestModemChatSnapshotLimit(t *testing.T) {
	e := testapi.New(t)
	e.Radio.SetModem(statusWith("DATAC3", `{"state":"idle","cycle":0,"cycles_total":0,"frames_sent":0}`))
	for i := 0; i < 150; i++ {
		e.Radio.AddChat("bcast", "PY2XYZ", "line "+strconv.Itoa(i))
	}
	w := e.Open()
	w.Send(`{"type":"subscribe","id":"s","topics":["modem.chat"]}`)
	first := w.Next(chatEvent, time.Second)
	if d := first["data"].(map[string]any); d["text"] != "line 50" {
		t.Errorf("first snapshot line = %v, want line 50", d["text"])
	}
	var last map[string]any
	for i := 1; i < 100; i++ {
		last = w.Next(chatEvent, time.Second)
	}
	if last["seq"] != 99.0 || last["data"].(map[string]any)["text"] != "line 149" {
		t.Errorf("last snapshot event = %v", last)
	}
	w.None(chatEvent, 300*time.Millisecond)
}
