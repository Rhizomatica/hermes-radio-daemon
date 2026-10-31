// Authentication and user management through the real daemon: radiod's C
// server hands each request, with its headers and peer address, to the Go
// layer, so this checks what the unit tests cannot: that sessions, the CSRF
// rule and the login limit survive the C/Go boundary.
//
// Copyright (C) 2026 Rhizomatica
// SPDX-License-Identifier: GPL-3.0-or-later

package integration

import (
	"encoding/json/v2"
	"io"
	"net/http"
	"strconv"
	"strings"
	"testing"
	"time"
)

// callHdr is callAuth with extra request headers (a Cookie, X-Hermes-CSRF, ...).
func callHdr(t *testing.T, method, url, body string, hdr map[string]string) httpResult {
	t.Helper()
	var rd io.Reader
	if body != "" {
		rd = strings.NewReader(body)
	}
	req, _ := http.NewRequest(method, url, rd)
	if body != "" {
		req.Header.Set("Content-Type", "application/json")
	}
	for k, v := range hdr {
		req.Header.Set(k, v)
	}
	resp, err := (&http.Client{Timeout: 5 * time.Second}).Do(req)
	if err != nil {
		t.Fatalf("%s %s: %v", method, url, err)
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	r := httpResult{status: resp.StatusCode, header: resp.Header, raw: string(b)}
	_ = json.Unmarshal(b, &r.body)
	return r
}

// wantStatus fails the test with the daemon's log if the reply is not status
// (and, for an error, not the problem code).
func wantStatus(t *testing.T, d *Daemon, what string, r httpResult, status int, code string) {
	t.Helper()
	if r.status != status || (code != "" && r.body["code"] != code) {
		t.Fatalf("%s = %d %s, want %d %s\n--- daemon log ---\n%s", what, r.status, r.raw, status, code, d.Log())
	}
}

func login(t *testing.T, d *Daemon, user, pass string) httpResult {
	t.Helper()
	return call(t, "POST", apiBase(d)+"/session", `{"username":"`+user+`","password":"`+pass+`"}`)
}

// The whole account lifecycle against the real daemon, in the order a station
// owner meets them: setup, log in, create / disable / re-enable / reset the
// password of / delete a user, the cookie and its CSRF rule, log out, and the
// failed-login limit.
func TestAPIv2AuthLifecycle(t *testing.T) {
	needGoAPI(t)
	d := startAPIDaemon(t)
	base := apiBase(d)

	// Before setup: not done, and nothing but the open operations answers.
	r := call(t, "GET", base+"/setup", "")
	if r.status != 200 || r.body["done"] != false {
		t.Fatalf("GET /setup before = %d %s", r.status, r.raw)
	}
	wantStatus(t, d, "GET /radio without a session", call(t, "GET", base+"/radio", ""), 401, "unauthenticated")

	admin := setupAPI(t, d)
	if r := call(t, "GET", base+"/setup", ""); r.body["done"] != true {
		t.Fatalf("GET /setup after = %s", r.raw)
	}
	wantStatus(t, d, "second POST /setup", call(t, "POST", base+"/setup",
		`{"setup_code":"`+apiSetupCode+`","station_name":"x","language":"en","timezone":"UTC","admin":{"username":"other","password":"password1"}}`),
		409, "setup_done")

	// Log in and ask who is logged in. The token is only in the login reply.
	r = login(t, d, "admin", "password1")
	wantStatus(t, d, "login", r, 201, "")
	token, _ := r.body["token"].(string)
	if token == "" || r.body["expires_at"] == nil {
		t.Fatalf("login reply = %s", r.raw)
	}
	r = callAuth(t, "GET", base+"/session", "", token)
	wantStatus(t, d, "GET /session", r, 200, "")
	if u, _ := r.body["user"].(map[string]any); u["username"] != "admin" || u["role"] != "admin" || r.body["token"] != nil {
		t.Errorf("GET /session = %s", r.raw)
	}
	wantStatus(t, d, "wrong password", login(t, d, "admin", "nope"), 401, "unauthenticated")

	// Create an operator; a user's own limits.
	r = callAuth(t, "POST", base+"/users", `{"username":"maria","password":"s3cret-pass","display_name":"Maria","role":"operator"}`, admin)
	wantStatus(t, d, "POST /users", r, 201, "")
	mariaID := int64(r.body["id"].(float64))
	idURL := base + "/users/" + strconv.FormatInt(mariaID, 10)
	wantStatus(t, d, "duplicate user", callAuth(t, "POST", base+"/users", `{"username":"maria","password":"s3cret-pass"}`, admin), 409, "conflict")
	wantStatus(t, d, "invalid user", callAuth(t, "POST", base+"/users", `{"username":"Bad Name","password":"x"}`, admin), 422, "invalid")

	r = login(t, d, "maria", "s3cret-pass")
	wantStatus(t, d, "maria login", r, 201, "")
	maria, _ := r.body["token"].(string)
	wantStatus(t, d, "maria GET /users", callAuth(t, "GET", base+"/users", "", maria), 403, "forbidden")
	wantStatus(t, d, "maria GET /radio", callAuth(t, "GET", base+"/radio", "", maria), 200, "")
	wantStatus(t, d, "maria PATCH own role", callAuth(t, "PATCH", idURL, `{"role":"admin"}`, maria), 403, "forbidden")
	wantStatus(t, d, "maria PATCH own name", callAuth(t, "PATCH", idURL, `{"display_name":"Maria S."}`, maria), 200, "")

	// Sessions: she sees hers, an admin sees hers too and may end it.
	r = callAuth(t, "GET", base+"/sessions", "", maria)
	wantStatus(t, d, "maria GET /sessions", r, 200, "")
	list, _ := r.body["data"].([]any)
	if len(list) != 1 || list[0].(map[string]any)["current"] != true {
		t.Fatalf("maria sessions = %s", r.raw)
	}
	wantStatus(t, d, "admin GET /sessions?user_id", callAuth(t, "GET", base+"/sessions?user_id="+strconv.FormatInt(mariaID, 10), "", admin), 200, "")

	// Disabled: her token stops at once and she cannot log in; enabling restores login.
	wantStatus(t, d, "disable", callAuth(t, "PATCH", idURL, `{"disabled":true}`, admin), 200, "")
	wantStatus(t, d, "disabled token", callAuth(t, "GET", base+"/session", "", maria), 401, "unauthenticated")
	wantStatus(t, d, "disabled login", login(t, d, "maria", "s3cret-pass"), 401, "unauthenticated")
	wantStatus(t, d, "enable", callAuth(t, "PATCH", idURL, `{"disabled":false}`, admin), 200, "")

	// A password reset by an admin ends all of her sessions.
	wantStatus(t, d, "login again", login(t, d, "maria", "s3cret-pass"), 201, "")
	wantStatus(t, d, "reset password", callAuth(t, "PUT", idURL+"/password", `{"new_password":"a-newer-passphrase"}`, admin), 204, "")
	wantStatus(t, d, "old password", login(t, d, "maria", "s3cret-pass"), 401, "unauthenticated")
	r = login(t, d, "maria", "a-newer-passphrase")
	wantStatus(t, d, "new password", r, 201, "")
	maria, _ = r.body["token"].(string)

	// Her own password change needs the current one and keeps her session.
	wantStatus(t, d, "self password, no current", callAuth(t, "PUT", idURL+"/password", `{"new_password":"yet-another-one"}`, maria), 422, "invalid")
	wantStatus(t, d, "self password", callAuth(t, "PUT", idURL+"/password",
		`{"current_password":"a-newer-passphrase","new_password":"yet-another-one"}`, maria), 204, "")
	wantStatus(t, d, "session kept after own change", callAuth(t, "GET", base+"/session", "", maria), 200, "")

	// The cookie: set by login, HttpOnly and SameSite=Strict; a write with it
	// needs Origin or X-Hermes-CSRF, a Bearer write does not.
	r = login(t, d, "maria", "yet-another-one")
	wantStatus(t, d, "cookie login", r, 201, "")
	setCookie := r.header.Get("Set-Cookie")
	if !strings.HasPrefix(setCookie, "hermes_session=") || !strings.Contains(setCookie, "HttpOnly") || !strings.Contains(setCookie, "SameSite=Strict") {
		t.Fatalf("Set-Cookie = %q", setCookie)
	}
	cookie := strings.SplitN(setCookie, ";", 2)[0]
	patch := `{"display_name":"Maria S."}`
	wantStatus(t, d, "cookie GET", callHdr(t, "GET", base+"/session", "", map[string]string{"Cookie": cookie}), 200, "")
	wantStatus(t, d, "cookie PATCH, no Origin", callHdr(t, "PATCH", idURL, patch, map[string]string{"Cookie": cookie}), 403, "csrf")
	wantStatus(t, d, "cookie PATCH, X-Hermes-CSRF", callHdr(t, "PATCH", idURL, patch,
		map[string]string{"Cookie": cookie, "X-Hermes-CSRF": "1"}), 200, "")
	wantStatus(t, d, "cookie PATCH, foreign Origin", callHdr(t, "PATCH", idURL, patch,
		map[string]string{"Cookie": cookie, "Origin": "http://evil.example"}), 403, "csrf")

	// Delete: root-less user gone, her token dead, a second delete is 404.
	wantStatus(t, d, "non-admin delete", callAuth(t, "DELETE", idURL, "", maria), 403, "forbidden")
	wantStatus(t, d, "delete", callAuth(t, "DELETE", idURL, "", admin), 204, "")
	wantStatus(t, d, "deleted user's token", callAuth(t, "GET", base+"/session", "", maria), 401, "unauthenticated")
	wantStatus(t, d, "GET deleted user", callAuth(t, "GET", idURL, "", admin), 404, "not_found")
	wantStatus(t, d, "delete again", callAuth(t, "DELETE", idURL, "", admin), 404, "not_found")

	// Log out ends this session only.
	other := login(t, d, "admin", "password1").body["token"].(string)
	wantStatus(t, d, "logout", callAuth(t, "DELETE", base+"/session", "", token), 204, "")
	wantStatus(t, d, "after logout", callAuth(t, "GET", base+"/session", "", token), 401, "unauthenticated")
	wantStatus(t, d, "other session survives", callAuth(t, "GET", base+"/session", "", other), 200, "")

	// Five failed logins a minute per address, then 429 with Retry-After.
	var limited httpResult
	for i := 0; i < 8 && limited.status != 429; i++ {
		limited = login(t, d, "admin", "wrong")
	}
	wantStatus(t, d, "login limit", limited, 429, "rate_limited")
	if limited.header.Get("Retry-After") == "" {
		t.Errorf("429 without Retry-After: %v", limited.header)
	}
}

// CORS through the real daemon (C passes OPTIONS and the headers through):
// the default allows any origin, a preflight is answered before the role
// gate, and credentials are never allowed.
func TestAPIv2CORS(t *testing.T) {
	needGoAPI(t)
	d := startAPIDaemon(t)
	base := apiBase(d)
	setupAPI(t, d)

	r := callHdr(t, "OPTIONS", base+"/radio/profiles/0", "", map[string]string{
		"Origin":                         "https://app.example.org",
		"Access-Control-Request-Method":  "PATCH",
		"Access-Control-Request-Headers": "authorization, content-type",
	})
	if r.status != 204 || r.header.Get("Access-Control-Allow-Origin") != "*" ||
		r.header.Get("Access-Control-Allow-Headers") != "authorization, content-type" {
		t.Fatalf("preflight = %d %v\n--- daemon log ---\n%s", r.status, r.header, d.Log())
	}
	if r.header.Get("Access-Control-Allow-Credentials") != "" {
		t.Fatal("credentials allowed cross-origin")
	}
	r = callHdr(t, "GET", base+"/radio", "", map[string]string{"Origin": "https://app.example.org"})
	if r.status != 401 || r.header.Get("Access-Control-Allow-Origin") != "*" {
		t.Fatalf("cross-origin GET without a session = %d %v", r.status, r.header)
	}
}
