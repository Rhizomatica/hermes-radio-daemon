// Package users serves the users operations (api-rest.yaml, tag users):
// the people with an account (and a mailbox) on this station.
//
// Avatars and LXMF identity export are not implemented.
package users

import (
	"database/sql"
	"errors"
	"net/http"
	"regexp"
	"strconv"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
)

// User is api-rest.yaml's User schema. avatar and lxmf_address are not
// served: there is no avatar support, and Reticulum is not implemented.
type User struct {
	ID          int64   `json:"id"`
	Username    string  `json:"username"`
	DisplayName *string `json:"display_name"`
	Role        string  `json:"role"`
	Locale      *string `json:"locale"`
	Phone       *string `json:"phone"`
	Location    *string `json:"location"`
	Disabled    bool    `json:"disabled"`
	LastLoginAt *string `json:"last_login_at"`
	CreatedAt   string  `json:"created_at"`
	UpdatedAt   string  `json:"updated_at"`
}

// Of maps a store.User to the API's User.
func Of(u store.User) User {
	return User{
		ID: u.ID, Username: u.Username, DisplayName: u.DisplayName, Role: u.Role,
		Locale: u.Locale, Phone: u.Phone, Location: u.Location, Disabled: u.Disabled,
		LastLoginAt: u.LastLoginAt, CreatedAt: u.CreatedAt, UpdatedAt: u.UpdatedAt,
	}
}

var (
	usernameRe = regexp.MustCompile(`^[a-z0-9._-]{1,32}$`)
	roles      = map[string]bool{"readonly": true, "user": true, "operator": true, "admin": true}
	locales    = map[string]bool{"en": true, "es": true, "pt-BR": true, "fr": true}
)

type handlers struct{ store *store.Store }

// Register adds the users operations.
func Register(rt *api.Router, st *store.Store) {
	h := &handlers{store: st}
	rt.Op("listUsers", h.list)
	rt.Op("createUser", h.create)
	rt.Op("getUser", h.get)
	rt.Op("updateUser", h.update)
	rt.Op("deleteUser", h.delete)
	rt.Op("setPassword", h.setPassword)
}

// userParam reads {user_id}; false after answering 404.
func userParam(w http.ResponseWriter, r *http.Request) (int64, bool) {
	id, err := strconv.ParseInt(r.PathValue("user_id"), 10, 64)
	if err != nil {
		api.Problem(w, r, http.StatusNotFound, "not_found", "no such user", nil)
		return 0, false
	}
	return id, true
}

// loadUser fetches a user or answers 404/500.
func (h *handlers) loadUser(w http.ResponseWriter, r *http.Request, id int64) (store.User, bool) {
	u, err := h.store.UserByID(r.Context(), id)
	if errors.Is(err, sql.ErrNoRows) {
		api.Problem(w, r, http.StatusNotFound, "not_found", "no such user", nil)
		return store.User{}, false
	}
	if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
		return store.User{}, false
	}
	return u, true
}
