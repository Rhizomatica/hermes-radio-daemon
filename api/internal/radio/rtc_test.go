package radio_test

import (
	"fmt"
	"strings"
	"testing"
	"time"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
)

const offerSDP = "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n" +
	"m=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:0\r\na=rtpmap:111 opus/48000/2\r\na=sendrecv\r\n"

// rtcCmd sends cmd with args on w and returns the reply.
func rtcCmd(w *testapi.WS, id, cmd, args string) map[string]any {
	w.Send(fmt.Sprintf(`{"type":"cmd","id":%q,"cmd":%q,"args":%s}`, id, cmd, args))
	return w.Next(func(m map[string]any) bool { return m["type"] == "reply" && m["id"] == id }, 2*time.Second)
}

func offerArgs(extra string) string {
	return fmt.Sprintf(`{"sdp":%q%s}`, offerSDP, extra)
}

// rtc.offer answers with the station's SDP, receive audio by default; the
// session ends with rtc.close, or with the connection.
func TestRTCOffer(t *testing.T) {
	e := testapi.New(t)
	e.Radio.RTCMax = 1
	a, b := e.Open(), e.Open()

	r := rtcCmd(a, "1", "rtc.offer", offerArgs(""))
	data, _ := r["data"].(map[string]any)
	if r["ok"] != true || !strings.HasPrefix(fmt.Sprint(data["sdp"]), "v=0") {
		t.Fatalf("offer = %v", r)
	}
	if s, ok := e.Radio.RTC(a.ID); !ok || !s.RX || s.TX || s.SDP != offerSDP {
		t.Fatalf("session = %+v, %v; want receive only, with the offer", s, ok)
	}
	if r := rtcCmd(b, "2", "rtc.offer", offerArgs("")); code(r) != "busy" {
		t.Fatalf("a session past the limit = %v, want busy", r)
	}
	// renegotiating keeps the one session, now with the microphone
	if r := rtcCmd(a, "3", "rtc.offer", offerArgs(`,"tx":true`)); r["ok"] != true {
		t.Fatalf("renegotiation = %v", r)
	}
	if s, _ := e.Radio.RTC(a.ID); !s.RX || !s.TX {
		t.Fatalf("renegotiated session = %+v, want rx and tx", s)
	}
	if r := rtcCmd(a, "4", "rtc.close", "{}"); r["ok"] != true {
		t.Fatalf("close = %v", r)
	}
	if _, ok := e.Radio.RTC(a.ID); ok {
		t.Fatal("rtc.close left the session")
	}
	if r := rtcCmd(b, "5", "rtc.offer", offerArgs("")); r["ok"] != true {
		t.Fatalf("offer once a session is free = %v", r)
	}
	// the connection's release runs after its last command
	b.Close()
	for deadline := time.Now().Add(time.Second); ; time.Sleep(time.Millisecond) {
		if _, ok := e.Radio.RTC(b.ID); !ok {
			break
		}
		if time.Now().After(deadline) {
			t.Fatal("closing the connection left its session")
		}
	}
}

func TestRTCOfferRefused(t *testing.T) {
	e := testapi.New(t)
	w := e.Open()
	if r := rtcCmd(w, "1", "rtc.offer", offerArgs("")); code(r) != "not_supported" {
		t.Fatalf("offer without WebRTC = %v, want not_supported", r)
	}
	e.Radio.RTCMax = 4
	for i, c := range []struct{ args, want string }{
		{`{}`, "missing_arg"},
		{`{"sdp":""}`, "missing_arg"},
		{`{"sdp":"v=0\r\nm=video 9 RTP/SAVPF 96\r\n"}`, "bad_request"},
		{offerArgs(`,"rx":false`), "out_of_range"},
		{offerArgs(`,"rx":false,"tx":false`), "out_of_range"},
		{fmt.Sprintf(`{"sdp":%q}`, offerSDP+strings.Repeat("a=x\r\n", 4000)), "out_of_range"},
		{offerArgs(`,"sdp":"again"`), "bad_request"},
	} {
		r := rtcCmd(w, fmt.Sprint(10+i), "rtc.offer", c.args)
		if code(r) != c.want {
			t.Errorf("args %.60s = %v, want %s", c.args, r, c.want)
		}
	}
}

// Sending microphone audio needs operator; a user receives only.
func TestRTCOfferTXNeedsOperator(t *testing.T) {
	e := testapi.NewAuth(t)
	e.Radio.RTCMax = 4
	e.SeedUser("listener", "correct horse battery", "user")
	w := e.OpenAuth(e.Login("listener", "correct horse battery"))
	if r := rtcCmd(w, "1", "rtc.offer", offerArgs(`,"tx":true`)); code(r) != "forbidden" {
		t.Fatalf("user with tx = %v, want forbidden", r)
	}
	if r := rtcCmd(w, "2", "rtc.offer", offerArgs("")); r["ok"] != true {
		t.Fatalf("user receiving = %v", r)
	}
}
