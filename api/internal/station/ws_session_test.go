package station_test

import (
	"net/http/httptest"
	"strconv"
	"testing"
	"time"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
)

// waitClosed waits for the station to close conn and returns the reason.
func waitClosed(t *testing.T, e *testapi.Env, w *testapi.WS) string {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if c, ok := e.Radio.Closed(w.ID); ok {
			if c.Code != 1008 {
				t.Fatalf("close code = %d, want 1008", c.Code)
			}
			return c.Reason
		}
		time.Sleep(5 * time.Millisecond)
	}
	t.Fatal("the connection was not closed")
	return ""
}

// A hermes.v1 connection lasts only as long as its session: after logout the
// next frame closes it (1008) instead of running.
func TestWSClosesWhenTheSessionEnds(t *testing.T) {
	e := testapi.NewAuth(t)
	e.SeedUser("op", "password1", "operator")
	token := e.Login("op", "password1")
	w := e.OpenAuth(token)
	w.Next(testapi.Type("hello"), time.Second)

	w.Send(`{"type":"cmd","id":"a","cmd":"radio.select_profile","args":{"profile":0}}`)
	w.Next(testapi.Type("reply"), 2*time.Second)

	if r := e.DoAuth("DELETE", "/session", "", token); r.Status != 204 {
		t.Fatalf("logout = %d %s", r.Status, r.Body)
	}
	w.Send(`{"type":"cmd","id":"b","cmd":"radio.select_profile","args":{"profile":0}}`)
	if why := waitClosed(t, e, w); why != "session ended" {
		t.Fatalf("reason = %q", why)
	}
	w.None(func(m map[string]any) bool { return m["id"] == "b" }, 200*time.Millisecond)
}

// A new role takes effect at once: the connection is closed, and the client
// connects again to get the hello of its new role.
func TestWSClosesWhenTheRoleChanges(t *testing.T) {
	e := testapi.NewAuth(t)
	e.SeedUser("root2", "password1", "admin")
	id := e.SeedUser("op", "password1", "operator")
	admin := e.Login("root2", "password1")
	token := e.Login("op", "password1")
	w := e.OpenAuth(token)
	w.Next(testapi.Type("hello"), time.Second)

	r := e.DoAuth("PATCH", "/users/"+strconv.FormatInt(id, 10), `{"role":"readonly"}`, admin)
	if r.Status != 200 {
		t.Fatalf("demote = %d %s", r.Status, r.Body)
	}
	w.Send(`{"type":"subscribe","id":"s","topics":["radio.state"]}`)
	if why := waitClosed(t, e, w); why != "role changed; connect again" {
		t.Fatalf("reason = %q", why)
	}
}

// A live session keeps its connection.
func TestWSKeepsALiveSession(t *testing.T) {
	e := testapi.NewAuth(t)
	e.SeedUser("op", "password1", "operator")
	w := e.OpenAuth(e.Login("op", "password1"))
	w.Next(testapi.Type("hello"), time.Second)
	w.Send(`{"type":"cmd","id":"a","cmd":"radio.select_profile","args":{"profile":0}}`)
	w.Next(testapi.Type("reply"), 2*time.Second)
	if _, closed := e.Radio.Closed(w.ID); closed {
		t.Fatal("a live session's connection was closed")
	}
}

// With sessions, a remote peer is answered by the role gate (401 without a
// session, the operation with one), never refused for its address.
func TestRemotePeerWithSessions(t *testing.T) {
	e := testapi.NewAuth(t)
	e.SeedUser("op", "password1", "operator")
	token := e.Login("op", "password1")
	bearer := "Authorization: Bearer " + token + "\r\n"

	if st, _, _, _ := e.R.ServeRaw("GET", "/api/v2/radio", "", "", "192.0.2.10", nil); st != 401 {
		t.Errorf("remote, no session = %d, want 401", st)
	}
	if st, _, _, b := e.R.ServeRaw("GET", "/api/v2/radio", "", bearer, "192.0.2.10", nil); st != 200 {
		t.Errorf("remote, with a session = %d %s, want 200", st, b)
	}
	if st, _, _, _ := e.R.ServeRaw("GET", "/api/v2/health", "", "", "192.0.2.10", nil); st != 200 {
		t.Errorf("remote, public operation = %d, want 200", st)
	}

	req := httptest.NewRequest("GET", "/api/v2/ws", nil)
	req.RemoteAddr = "192.0.2.10"
	if st := e.R.WSOpen(9001, req); st != 401 {
		t.Errorf("remote upgrade, no session = %d, want 401", st)
	}
	w := e.OpenAuth(token) // a remote peer with a session is accepted
	w.Next(testapi.Type("hello"), time.Second)
}
