package users

import (
	"database/sql"
	"encoding/json/jsontext"
	"encoding/json/v2"
	"errors"
	"fmt"
	"net/http"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/passwd"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
	"github.com/Rhizomatica/hermes-radio-daemon/api/spec"
)

func (h *handlers) update(w http.ResponseWriter, r *http.Request) {
	id, ok := userParam(w, r)
	if !ok {
		return
	}
	p, _ := api.PrincipalFrom(r)
	isAdmin := p.Role == spec.RoleAdmin
	if !isAdmin && p.UserID != id {
		api.Problem(w, r, http.StatusForbidden, "forbidden", "you may change only your own profile", nil)
		return
	}
	u, ok := h.loadUser(w, r, id)
	if !ok {
		return
	}

	var raw map[string]jsontext.Value
	if !api.Decode(w, r, &raw) {
		return
	}
	f := api.Fields{}
	known := map[string]bool{"display_name": true, "role": true, "locale": true, "phone": true, "location": true, "disabled": true}
	for k := range raw {
		if !known[k] {
			f.Add(k, "unknown field")
		}
	}
	if !f.OK() {
		api.Invalid(w, r, f)
		return
	}

	var patch store.UserPatch
	if v, present := raw["display_name"]; present {
		var s string
		if json.Unmarshal(v, &s) != nil || !store.ValidDisplayName(s) {
			f.Add("display_name", fmt.Sprintf("a string of at most %d characters", store.MaxDisplayName))
		} else {
			patch.DisplayName = &s
		}
	}
	if v, present := raw["role"]; present {
		var s string
		if json.Unmarshal(v, &s) != nil || !roles[s] {
			f.Add("role", "readonly, user, operator or admin")
		} else {
			patch.Role = &s
		}
	}
	if v, present := raw["locale"]; present {
		var s string
		if json.Unmarshal(v, &s) != nil || !locales[s] {
			f.Add("locale", "en, es, pt-BR or fr")
		} else {
			patch.Locale = &s
		}
	}
	if v, present := raw["phone"]; present {
		if s, isNull, err := nullableString(v); err != nil {
			f.Add("phone", "a string or null")
		} else if isNull {
			patch.ClearPhone = true
		} else {
			patch.Phone = s
		}
	}
	if v, present := raw["location"]; present {
		if s, isNull, err := nullableString(v); err != nil {
			f.Add("location", "a string or null")
		} else if isNull {
			patch.ClearLocation = true
		} else {
			patch.Location = s
		}
	}
	if v, present := raw["disabled"]; present {
		var b bool
		if json.Unmarshal(v, &b) != nil {
			f.Add("disabled", "a boolean")
		} else {
			patch.Disabled = &b
		}
	}

	// role, locale and disabled are admin-only.
	if !isAdmin && (patch.Role != nil || patch.Locale != nil || patch.Disabled != nil) {
		api.Problem(w, r, http.StatusForbidden, "forbidden", "only an admin may change role, locale or disabled", nil)
		return
	}
	if !f.OK() {
		api.Invalid(w, r, f)
		return
	}
	// root stays an admin; restoring that role after an upgrade is allowed.
	if u.Username == store.RootUsername && patch.Role != nil && *patch.Role != "admin" {
		api.Problem(w, r, http.StatusForbidden, "forbidden", "root must stay an admin", nil)
		return
	}
	// Refuse to leave the station without an enabled admin.
	keepActiveAdmin := false
	if u.Role == "admin" && !u.Disabled {
		newRole, newDisabled := u.Role, u.Disabled
		if patch.Role != nil {
			newRole = *patch.Role
		}
		if patch.Disabled != nil {
			newDisabled = *patch.Disabled
		}
		keepActiveAdmin = newRole != "admin" || newDisabled
	}

	var updated store.User
	var err error
	if keepActiveAdmin {
		updated, err = h.store.UpdateUserKeepingActiveAdmin(r.Context(), id, patch)
	} else {
		updated, err = h.store.UpdateUser(r.Context(), id, patch)
	}
	if errors.Is(err, store.ErrLastAdmin) {
		api.Problem(w, r, http.StatusForbidden, "forbidden", "cannot remove the last active admin", nil)
		return
	}
	if errors.Is(err, sql.ErrNoRows) {
		api.Problem(w, r, http.StatusNotFound, "not_found", "no such user", nil)
		return
	}
	if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
		return
	}
	api.JSON(w, http.StatusOK, Of(updated))
}

func (h *handlers) delete(w http.ResponseWriter, r *http.Request) {
	id, ok := userParam(w, r)
	if !ok {
		return
	}
	u, ok := h.loadUser(w, r, id)
	if !ok {
		return
	}
	if u.Username == store.RootUsername {
		api.Problem(w, r, http.StatusForbidden, "forbidden", "root cannot be deleted", nil)
		return
	}
	var err error
	if u.Role == "admin" && !u.Disabled {
		err = h.store.DeleteUserKeepingActiveAdmin(r.Context(), id)
	} else {
		err = h.store.DeleteUser(r.Context(), id)
	}
	if errors.Is(err, store.ErrLastAdmin) {
		api.Problem(w, r, http.StatusForbidden, "forbidden", "cannot delete the last active admin", nil)
		return
	}
	if errors.Is(err, sql.ErrNoRows) {
		api.Problem(w, r, http.StatusNotFound, "not_found", "no such user", nil)
		return
	}
	if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
		return
	}
	api.JSON(w, http.StatusNoContent, nil)
}

func (h *handlers) setPassword(w http.ResponseWriter, r *http.Request) {
	id, ok := userParam(w, r)
	if !ok {
		return
	}
	p, _ := api.PrincipalFrom(r)
	isAdmin := p.Role == spec.RoleAdmin
	if !isAdmin && p.UserID != id {
		api.Problem(w, r, http.StatusForbidden, "forbidden", "you may change only your own password", nil)
		return
	}
	u, ok := h.loadUser(w, r, id)
	if !ok {
		return
	}

	var req struct {
		CurrentPassword string `json:"current_password"`
		NewPassword     string `json:"new_password"`
	}
	if !api.Decode(w, r, &req) {
		return
	}
	f := api.Fields{}
	if len(req.NewPassword) < store.MinPasswordLength {
		f.Add("new_password", fmt.Sprintf("at least %d characters", store.MinPasswordLength))
	}
	if !isAdmin {
		if req.CurrentPassword == "" {
			f.Add("current_password", "required")
		} else if !passwd.Verify(u.PasswordHash, req.CurrentPassword) {
			f.Add("current_password", "wrong password")
		}
	}
	if !f.OK() {
		api.Invalid(w, r, f)
		return
	}
	hash, err := passwd.Hash(req.NewPassword)
	if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "cannot hash the password", nil)
		return
	}
	if err := h.store.SetPasswordHash(r.Context(), id, hash); errors.Is(err, sql.ErrNoRows) {
		api.Problem(w, r, http.StatusNotFound, "not_found", "no such user", nil)
		return
	} else if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
		return
	}
	// Other sessions of that user end (api-rest.yaml setPassword); a user
	// changing their own password keeps the session making this request.
	// The mailbox update arrives with the email helper (not implemented yet).
	if p.UserID == id {
		if err := h.store.RevokeAllForUserExcept(r.Context(), id, p.SessionID); err != nil {
			api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
			return
		}
	} else if err := h.store.RevokeAllForUser(r.Context(), id); err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
		return
	}
	api.JSON(w, http.StatusNoContent, nil)
}

// nullableString reads a JSON string or null.
func nullableString(v jsontext.Value) (*string, bool, error) {
	if string(v) == "null" {
		return nil, true, nil
	}
	var s string
	if err := json.Unmarshal(v, &s); err != nil {
		return nil, false, err
	}
	return &s, false, nil
}
