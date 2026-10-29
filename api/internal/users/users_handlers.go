package users

import (
	"database/sql"
	"errors"
	"net/http"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/passwd"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
	"github.com/Rhizomatica/hermes-radio-daemon/api/spec"
)

func (h *handlers) list(w http.ResponseWriter, r *http.Request) {
	limit, ok := api.Limit(w, r)
	if !ok {
		return
	}
	cursor, ok := api.DecodeCursor(w, r)
	if !ok {
		return
	}
	list, next, err := h.store.ListUsers(r.Context(), limit, int64(cursor))
	if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
		return
	}
	out := api.Page[User]{Data: make([]User, 0, len(list))}
	for _, u := range list {
		out.Data = append(out.Data, Of(u))
	}
	if next != 0 {
		out.NextCursor = api.EncodeCursor(uint64(next))
	}
	api.JSON(w, http.StatusOK, out)
}

func (h *handlers) create(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Username    string  `json:"username"`
		Password    string  `json:"password"`
		DisplayName *string `json:"display_name"`
		Role        string  `json:"role"`
		Locale      *string `json:"locale"`
		Phone       *string `json:"phone"`
		Location    *string `json:"location"`
	}
	if !api.Decode(w, r, &req) {
		return
	}
	f := api.Fields{}
	if !usernameRe.MatchString(req.Username) {
		f.Add("username", "1 to 32 characters: a-z, 0-9, . _ -")
	}
	if len(req.Password) < 8 {
		f.Add("password", "at least 8 characters")
	}
	role := req.Role
	if role == "" {
		role = "user"
	}
	if !roles[role] {
		f.Add("role", "readonly, user, operator or admin")
	}
	if req.DisplayName != nil && len(*req.DisplayName) > 64 {
		f.Add("display_name", "at most 64 characters")
	}
	if req.Locale != nil && !locales[*req.Locale] {
		f.Add("locale", "en, es, pt-BR or fr")
	}
	if !f.OK() {
		api.Invalid(w, r, f)
		return
	}
	if _, err := h.store.UserByUsername(r.Context(), req.Username); err == nil {
		api.Problem(w, r, http.StatusConflict, "conflict", "username is taken", nil)
		return
	} else if !errors.Is(err, sql.ErrNoRows) {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
		return
	}
	hash, err := passwd.Hash(req.Password)
	if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "cannot hash the password", nil)
		return
	}
	u, err := h.store.CreateUser(r.Context(), store.NewUser{
		Username: req.Username, PasswordHash: hash, Role: role,
		DisplayName: req.DisplayName, Locale: req.Locale, Phone: req.Phone, Location: req.Location,
	})
	if err != nil {
		if errors.Is(err, store.ErrUsernameTaken) {
			api.Problem(w, r, http.StatusConflict, "conflict", "username is taken", nil)
		} else {
			api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
		}
		return
	}
	api.JSON(w, http.StatusCreated, Of(u))
}

func (h *handlers) get(w http.ResponseWriter, r *http.Request) {
	id, ok := userParam(w, r)
	if !ok {
		return
	}
	u, ok := h.loadUser(w, r, id)
	if !ok {
		return
	}
	p, _ := api.PrincipalFrom(r)
	if p.Role != spec.RoleAdmin && p.UserID != id {
		api.Problem(w, r, http.StatusForbidden, "forbidden", "you may see only your own profile", nil)
		return
	}
	api.JSON(w, http.StatusOK, Of(u))
}
