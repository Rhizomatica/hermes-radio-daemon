package users_test

import (
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

	// Self change with the current password; other sessions end.
	if r := e.DoAuth("PUT", "/users/"+itoa(id)+"/password", `{"current_password":"password1","new_password":"newpass99"}`, bt); r.Status != 204 {
		t.Fatalf("self setPassword = %d: %s", r.Status, r.Body)
	}
	if r := e.DoAuth("GET", "/session", "", bt); r.Status != 401 {
		t.Fatalf("old session survived password change: %d", r.Status)
	}
	if r := e.Do("POST", "/session", `{"username":"bob","password":"password1"}`); r.Status != 401 {
		t.Fatalf("old password still works: %d", r.Status)
	}
	e.Login("bob", "newpass99")

	// Wrong current password → 422.
	bt2 := e.Login("bob", "newpass99")
	if r := e.DoAuth("PUT", "/users/"+itoa(id)+"/password", `{"current_password":"wrong","new_password":"newpass100"}`, bt2); r.Status != 422 {
		t.Fatalf("wrong current = %d, want 422", r.Status)
	}
	// An admin changes without the current password.
	if r := e.DoAuth("PUT", "/users/"+itoa(id)+"/password", `{"new_password":"adminpass1"}`, at); r.Status != 204 {
		t.Fatalf("admin setPassword = %d", r.Status)
	}
	e.Login("bob", "adminpass1")
}
