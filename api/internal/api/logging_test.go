package api_test

import (
	"net/http"
	"strings"
	"testing"
	"time"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
)

func logged(s *rad.Stub, prefix string) string {
	for _, l := range s.Logs() {
		if strings.HasPrefix(l, prefix) {
			return l
		}
	}
	return ""
}

// One line per request on radiod's logger: debug for a read that worked,
// info for a write or a refusal, with the status, the time and the id.
func TestRequestLines(t *testing.T) {
	e := testapi.New(t)
	e.Do("GET", "/radio", "")
	r := e.Do("PATCH", "/radio/profiles/0", `{"power":300}`)
	e.Do("GET", "/nowhere", "")

	if l := logged(e.Radio, "[DBG] [api.http] GET /api/v2/radio 200 ms="); l == "" {
		t.Errorf("no debug line for GET /radio: %q", e.Radio.Logs())
	}
	l := logged(e.Radio, "[INF] [api.http] PATCH /api/v2/radio/profiles/0 422 ms=")
	if l == "" || !strings.HasSuffix(l, " request_id="+r.Header.Get("X-Request-Id")) {
		t.Errorf("PATCH line = %q (request id %s)", l, r.Header.Get("X-Request-Id"))
	}
	if logged(e.Radio, "[INF] [api.http] GET /api/v2/nowhere 404") == "" {
		t.Errorf("no line for a 404: %q", e.Radio.Logs())
	}
}

// A handler that panics is answered with a 500 and logged as an error.
func TestPanicLogged(t *testing.T) {
	s := rad.NewStub()
	rt := api.NewRouter(s)
	rt.Op("getHealth", func(http.ResponseWriter, *http.Request) { panic("boom") })
	status, _, _, _ := rt.ServeRaw("GET", "/api/v2/health", "", "", "127.0.0.1", nil)
	if status != 500 {
		t.Fatalf("status = %d", status)
	}
	if l := logged(s, "[ERR] [api.http] GET /api/v2/health: the handler panicked panic=boom"); l == "" {
		t.Errorf("panic not logged: %q", s.Logs())
	}
}

// hermes.v1: connections opening and closing, and each command (debug).
func TestWSLines(t *testing.T) {
	e := testapi.New(t)
	w := e.Open()
	w.Send(`{"type":"cmd","id":"c1","cmd":"radio.select_profile","args":{"profile":7}}`)
	w.Next(testapi.Type("reply"), time.Second)
	deadline := time.Now().Add(time.Second) // the line follows the reply
	for logged(e.Radio, "[DBG] [api.ws] cmd ") == "" && time.Now().Before(deadline) {
		time.Sleep(5 * time.Millisecond)
	}
	e.R.WSClose(w.ID)
	for _, p := range []string{
		"[INF] [api.ws] open conn=",
		"[DBG] [api.ws] cmd radio.select_profile conn=",
		"[INF] [api.ws] closed conn=",
	} {
		if logged(e.Radio, p) == "" {
			t.Errorf("no line %q in %q", p, e.Radio.Logs())
		}
	}
	if l := logged(e.Radio, "[DBG] [api.ws] cmd radio.select_profile"); !strings.Contains(l, " ok=false code=out_of_range ") {
		t.Errorf("command line = %q", l)
	}
}
