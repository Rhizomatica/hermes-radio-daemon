package auth_test

import (
	"strconv"
	"testing"
)

func TestRoleEnforcement(t *testing.T) {
	rt, st := newEnv(t)
	seedUser(t, st, "ro", "password1", "readonly")

	w := do(rt, "POST", "/api/v2/session", `{"username":"ro","password":"password1"}`, "")
	token := jsonBody(t, w)["token"].(string)

	// A public operation needs no session.
	if w := do(rt, "GET", "/api/v2/health", "", ""); w.Code != 200 {
		t.Fatalf("getHealth = %d", w.Code)
	}
	// A protected operation without a session is 401 unauthenticated.
	if w := do(rt, "GET", "/api/v2/session", "", ""); w.Code != 401 {
		t.Fatalf("getSession with no token = %d, want 401", w.Code)
	}
	// readonly may read the radio but not reset protection.
	if w := do(rt, "GET", "/api/v2/radio", "", token); w.Code != 200 {
		t.Fatalf("readonly GET /radio = %d", w.Code)
	}
	if w := do(rt, "POST", "/api/v2/radio/protection/reset", "", token); w.Code != 403 {
		t.Fatalf("readonly resetProtection = %d, want 403", w.Code)
	}
}

func TestListAndRevokeSessions(t *testing.T) {
	rt, st := newEnv(t)
	seedUser(t, st, "alice", "password1", "admin")

	w := do(rt, "POST", "/api/v2/session", `{"username":"alice","password":"password1"}`, "")
	token := jsonBody(t, w)["token"].(string)
	w = do(rt, "POST", "/api/v2/session", `{"username":"alice","password":"password1"}`, "")
	token2 := jsonBody(t, w)["token"].(string)

	w = do(rt, "GET", "/api/v2/sessions", "", token)
	if w.Code != 200 {
		t.Fatalf("listMySessions = %d: %s", w.Code, w.Body.Bytes())
	}
	data := jsonBody(t, w)["data"].([]any)
	if len(data) != 2 {
		t.Fatalf("sessions = %d, want 2", len(data))
	}
	current := 0
	for _, s := range data {
		if s.(map[string]any)["current"] == true {
			current++
		}
	}
	if current != 1 {
		t.Fatalf("current sessions = %d, want 1", current)
	}

	// Revoke the other session; it stops working.
	other := int64(0)
	for _, s := range data {
		if s.(map[string]any)["current"] != true {
			other = int64(s.(map[string]any)["id"].(float64))
		}
	}
	if other == 0 {
		t.Fatalf("no non-current session: %v", data)
	}
	if w := do(rt, "DELETE", "/api/v2/sessions/"+strconv.FormatInt(other, 10), "", token); w.Code != 204 {
		t.Fatalf("revokeSession = %d: %s", w.Code, w.Body.Bytes())
	}
	if w := do(rt, "GET", "/api/v2/session", "", token2); w.Code != 401 {
		t.Fatalf("revoked session still works: %d", w.Code)
	}
}
