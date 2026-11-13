package users_test

import (
	"strings"
	"testing"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
)

func TestUpdateUser(t *testing.T) {
	e := testapi.NewAuth(t)
	e.SeedUser("admin", "password1", "admin")
	id := e.SeedUser("bob", "password1", "user")
	at := e.Login("admin", "password1")
	bt := e.Login("bob", "password1")

	// A user may change their own display name.
	r := e.DoAuth("PATCH", "/users/"+itoa(id), `{"display_name":"Bobby"}`, bt)
	if r.Status != 200 || r.JSON(t)["display_name"] != "Bobby" {
		t.Fatalf("self update = %d %s", r.Status, r.Body)
	}
	// But not their role, locale or disabled.
	if r := e.DoAuth("PATCH", "/users/"+itoa(id), `{"role":"admin"}`, bt); r.Status != 403 {
		t.Fatalf("self role = %d, want 403", r.Status)
	}
	// An admin may.
	r = e.DoAuth("PATCH", "/users/"+itoa(id), `{"role":"operator","disabled":true}`, at)
	if r.Status != 200 {
		t.Fatalf("admin update = %d: %s", r.Status, r.Body)
	}
	if j := r.JSON(t); j["role"] != "operator" || j["disabled"] != true {
		t.Fatalf("updated = %v", j)
	}
	// A disabled user can no longer log in.
	if r := e.Do("POST", "/session", `{"username":"bob","password":"password1"}`); r.Status != 401 {
		t.Fatalf("disabled login = %d, want 401", r.Status)
	}
	// Phone may be cleared with null.
	if r := e.DoAuth("PATCH", "/users/"+itoa(id), `{"phone":null}`, at); r.Status != 200 || r.JSON(t)["phone"] != nil {
		t.Fatalf("clear phone = %d %s", r.Status, r.Body)
	}
}

func TestDeleteUser(t *testing.T) {
	e := testapi.NewAuth(t)
	e.SeedUser("admin", "password1", "admin")
	id := e.SeedUser("bob", "password1", "user")
	rid := e.SeedUser("root", "password1", "admin")
	at := e.Login("admin", "password1")

	if r := e.DoAuth("DELETE", "/users/"+itoa(rid), "", at); r.Status != 403 {
		t.Fatalf("delete root = %d, want 403", r.Status)
	}
	if r := e.DoAuth("DELETE", "/users/"+itoa(id), "", at); r.Status != 204 {
		t.Fatalf("deleteUser = %d", r.Status)
	}
	if r := e.DoAuth("GET", "/users/"+itoa(id), "", at); r.Status != 404 {
		t.Fatalf("get deleted = %d, want 404", r.Status)
	}
}

func TestSetPassword(t *testing.T) {
	e := testapi.NewAuth(t)
	e.SeedUser("admin", "password1", "admin")
	id := e.SeedUser("bob", "password1", "user")
	at := e.Login("admin", "password1")
	bt := e.Login("bob", "password1")
	bt2 := e.Login("bob", "password1") // a second session, to be revoked

	// Self change with the current password; other sessions end, but the
	// current one survives (api-rest.yaml setPassword).
	if r := e.DoAuth("PUT", "/users/"+itoa(id)+"/password", `{"current_password":"password1","new_password":"newpass99"}`, bt); r.Status != 204 {
		t.Fatalf("self setPassword = %d: %s", r.Status, r.Body)
	}
	if r := e.DoAuth("GET", "/session", "", bt); r.Status != 200 {
		t.Fatalf("current session did not survive password change: %d", r.Status)
	}
	if r := e.DoAuth("GET", "/session", "", bt2); r.Status != 401 {
		t.Fatalf("other session survived password change: %d", r.Status)
	}
	if r := e.Do("POST", "/session", `{"username":"bob","password":"password1"}`); r.Status != 401 {
		t.Fatalf("old password still works: %d", r.Status)
	}
	e.Login("bob", "newpass99")

	// Wrong current password → 422.
	bt3 := e.Login("bob", "newpass99")
	if r := e.DoAuth("PUT", "/users/"+itoa(id)+"/password", `{"current_password":"wrong","new_password":"newpass100"}`, bt3); r.Status != 422 {
		t.Fatalf("wrong current = %d, want 422", r.Status)
	}
	// An admin changes without the current password.
	if r := e.DoAuth("PUT", "/users/"+itoa(id)+"/password", `{"new_password":"adminpass1"}`, at); r.Status != 204 {
		t.Fatalf("admin setPassword = %d", r.Status)
	}
	e.Login("bob", "adminpass1")
}

func TestUpdateUserDisplayNameCountsCharacters(t *testing.T) {
	e := testapi.NewAuth(t)
	e.SeedUser("admin", "password1", "admin")
	at := e.Login("admin", "password1")

	// 64 multibyte characters are fine even though they are more than 64
	// bytes; 65 are not.
	body := `{"username":"bob","password":"password1","display_name":"` + strings.Repeat("é", 64) + `"}`
	if r := e.DoAuth("POST", "/users", body, at); r.Status != 201 {
		t.Fatalf("64-char create = %d: %s", r.Status, r.Body)
	}
	body = `{"username":"carol","password":"password1","display_name":"` + strings.Repeat("é", 65) + `"}`
	if r := e.DoAuth("POST", "/users", body, at); r.Status != 422 {
		t.Fatalf("65-char create = %d, want 422", r.Status)
	}
}

func TestLastAdminGuards(t *testing.T) {
	e := testapi.NewAuth(t)
	id := e.SeedUser("admin", "password1", "admin")
	at := e.Login("admin", "password1")

	// The only active admin cannot demote or disable itself.
	if r := e.DoAuth("PATCH", "/users/"+itoa(id), `{"role":"user"}`, at); r.Status != 403 {
		t.Fatalf("demote last admin = %d, want 403", r.Status)
	}
	if r := e.DoAuth("PATCH", "/users/"+itoa(id), `{"disabled":true}`, at); r.Status != 403 {
		t.Fatalf("disable last admin = %d, want 403", r.Status)
	}
	// Nor delete itself.
	if r := e.DoAuth("DELETE", "/users/"+itoa(id), "", at); r.Status != 403 {
		t.Fatalf("delete self = %d, want 403", r.Status)
	}

	// With a second admin, self-demotion is allowed.
	e.SeedUser("admin2", "password1", "admin")
	if r := e.DoAuth("PATCH", "/users/"+itoa(id), `{"role":"operator"}`, at); r.Status != 200 {
		t.Fatalf("demote with another admin = %d: %s", r.Status, r.Body)
	}
}

func TestReadonlyCanChangeOwnPassword(t *testing.T) {
	e := testapi.NewAuth(t)
	e.SeedUser("admin", "password1", "admin")
	id := e.SeedUser("reader", "password1", "readonly")
	rt := e.Login("reader", "password1")

	if r := e.DoAuth("PUT", "/users/"+itoa(id)+"/password", `{"current_password":"password1","new_password":"newpass99"}`, rt); r.Status != 204 {
		t.Fatalf("readonly setPassword = %d: %s", r.Status, r.Body)
	}
	e.Login("reader", "newpass99")
}
