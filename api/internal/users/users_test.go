package users_test

import (
	"strconv"
	"testing"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
)

func itoa(n int64) string { return strconv.FormatInt(n, 10) }

func TestCreateAndListUsers(t *testing.T) {
	e := testapi.NewAuth(t)
	e.SeedUser("admin", "password1", "admin")
	at := e.Login("admin", "password1")

	r := e.DoAuth("POST", "/users", `{"username":"bob","password":"password1","role":"user","display_name":"Bob"}`, at)
	if r.Status != 201 {
		t.Fatalf("createUser = %d: %s", r.Status, r.Body)
	}
	if j := r.JSON(t); j["username"] != "bob" || j["role"] != "user" || j["display_name"] != "Bob" {
		t.Fatalf("created = %v", j)
	}
	// Duplicate username → 409.
	if r := e.DoAuth("POST", "/users", `{"username":"bob","password":"password1"}`, at); r.Status != 409 {
		t.Fatalf("duplicate = %d, want 409", r.Status)
	}
	// Bad username and short password → 422.
	if r := e.DoAuth("POST", "/users", `{"username":"B ob","password":"short"}`, at); r.Status != 422 {
		t.Fatalf("invalid create = %d, want 422", r.Status)
	}

	r = e.DoAuth("GET", "/users", "", at)
	if r.Status != 200 {
		t.Fatalf("listUsers = %d", r.Status)
	}
	if data := r.JSON(t)["data"].([]any); len(data) != 2 {
		t.Fatalf("listUsers data = %d, want 2", len(data))
	}

	// A non-admin cannot list.
	e.SeedUser("user1", "password1", "user")
	ut := e.Login("user1", "password1")
	if r := e.DoAuth("GET", "/users", "", ut); r.Status != 403 {
		t.Fatalf("non-admin listUsers = %d, want 403", r.Status)
	}
}

func TestGetUserPermissions(t *testing.T) {
	e := testapi.NewAuth(t)
	e.SeedUser("admin", "password1", "admin")
	id := e.SeedUser("bob", "password1", "user")
	at := e.Login("admin", "password1")
	bt := e.Login("bob", "password1")

	if r := e.DoAuth("GET", "/users/"+itoa(id), "", bt); r.Status != 200 {
		t.Fatalf("getUser self = %d", r.Status)
	}
	if r := e.DoAuth("GET", "/users/"+itoa(id), "", at); r.Status != 200 {
		t.Fatalf("getUser as admin = %d", r.Status)
	}

	e.SeedUser("carol", "password1", "user")
	ct := e.Login("carol", "password1")
	if r := e.DoAuth("GET", "/users/"+itoa(id), "", ct); r.Status != 403 {
		t.Fatalf("getUser other = %d, want 403", r.Status)
	}
	if r := e.DoAuth("GET", "/users/999", "", at); r.Status != 404 {
		t.Fatalf("getUser missing = %d, want 404", r.Status)
	}
}
