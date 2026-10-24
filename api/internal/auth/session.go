package auth

import (
	"database/sql"
	"errors"
	"net/http"
	"strconv"
	"time"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/passwd"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/users"
)

// sessionCookie is the Set-Cookie line api-rest.yaml specifies.
func sessionCookie(value string, maxAge int) *http.Cookie {
	return &http.Cookie{
		Name: "hermes_session", Value: value, Path: "/",
		HttpOnly: true, Secure: true, SameSite: http.SameSiteStrictMode, MaxAge: maxAge,
	}
}

func (h *handlers) login(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if !api.Decode(w, r, &req) {
		return
	}
	key := "login:" + clientIP(r)
	if ok, retry := h.loginLimiter.Allow(key, 5, time.Minute); !ok {
		w.Header().Set("Retry-After", strconv.Itoa(int(retry.Seconds())+1))
		api.Problem(w, r, http.StatusTooManyRequests, "rate_limited", "too many failed logins", nil)
		return
	}
	u, err := h.store.UserByUsername(r.Context(), req.Username)
	if errors.Is(err, sql.ErrNoRows) {
		h.dummyVerify(req.Password)
	} else if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
		return
	}
	if err == nil && !u.Disabled && passwd.Verify(u.PasswordHash, req.Password) {
		h.startSession(w, r, u)
		return
	}
	h.loginLimiter.Hit(key)
	api.Problem(w, r, http.StatusUnauthorized, "unauthenticated", "wrong username or password", nil)
}

func (h *handlers) startSession(w http.ResponseWriter, r *http.Request, u store.User) {
	token, expires, err := IssueSession(w, r, h.store, h.lifetime, u)
	if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "cannot make a session token", nil)
		return
	}
	api.JSON(w, http.StatusCreated, Session{
		User: users.Of(u), Token: token, ExpiresAt: expires,
	})
}

// IssueSession creates a session for u over st, sets the hermes_session
// cookie and returns the token and its expiry. The first-boot setup uses it
// to log the new admin in.
func IssueSession(w http.ResponseWriter, r *http.Request, st *store.Store, lifetime time.Duration, u store.User) (string, string, error) {
	raw, hash, err := newToken()
	if err != nil {
		return "", "", err
	}
	expires := time.Now().Add(lifetime).UTC()
	expiresAt := expires.Format(time.RFC3339)
	if _, err := st.CreateSession(r.Context(), store.NewSession{
		UserID: u.ID, TokenHash: hash, IP: clientIP(r), UserAgent: r.UserAgent(), ExpiresAt: expiresAt,
	}); err != nil {
		return "", "", err
	}
	_ = st.SetLastLoginAt(r.Context(), u.ID)
	http.SetCookie(w, sessionCookie(raw, int(lifetime.Seconds())))
	return raw, expiresAt, nil
}

func (h *handlers) getSession(w http.ResponseWriter, r *http.Request) {
	a, ok := h.resolve(r)
	if !ok {
		api.Problem(w, r, http.StatusUnauthorized, "unauthenticated", "not logged in", nil)
		return
	}
	u, err := h.store.UserByID(r.Context(), a.UserID)
	if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
		return
	}
	api.JSON(w, http.StatusOK, Session{User: users.Of(u), ExpiresAt: a.ExpiresAt})
}

func (h *handlers) logout(w http.ResponseWriter, r *http.Request) {
	a, ok := h.resolve(r)
	if !ok {
		api.Problem(w, r, http.StatusUnauthorized, "unauthenticated", "not logged in", nil)
		return
	}
	if err := h.store.RevokeSession(r.Context(), a.SessionID); err != nil && !errors.Is(err, sql.ErrNoRows) {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
		return
	}
	http.SetCookie(w, sessionCookie("", -1))
	api.JSON(w, http.StatusNoContent, nil)
}

func (h *handlers) listMySessions(w http.ResponseWriter, r *http.Request) {
	a, ok := h.resolve(r)
	if !ok {
		api.Problem(w, r, http.StatusUnauthorized, "unauthenticated", "not logged in", nil)
		return
	}
	target := a.UserID
	if q := r.URL.Query().Get("user_id"); q != "" {
		if a.Role != "admin" {
			api.Problem(w, r, http.StatusForbidden, "forbidden", "only admins may list another user's sessions", nil)
			return
		}
		id, err := strconv.ParseInt(q, 10, 64)
		if err != nil {
			api.Invalid(w, r, api.Fields{"user_id": {"an integer"}})
			return
		}
		target = id
	}
	list, err := h.store.ListSessions(r.Context(), target)
	if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
		return
	}
	out := make([]sessionInfo, 0, len(list))
	for _, s := range list {
		out = append(out, sessionInfo{
			ID: s.ID, UserID: s.UserID, CreatedAt: s.CreatedAt, LastSeenAt: s.LastSeenAt,
			ExpiresAt: s.ExpiresAt, IP: s.IP, UserAgent: s.UserAgent, Current: s.ID == a.SessionID,
		})
	}
	api.JSON(w, http.StatusOK, map[string]any{"data": out})
}

func (h *handlers) revokeSession(w http.ResponseWriter, r *http.Request) {
	a, ok := h.resolve(r)
	if !ok {
		api.Problem(w, r, http.StatusUnauthorized, "unauthenticated", "not logged in", nil)
		return
	}
	id, err := strconv.ParseInt(r.PathValue("session_id"), 10, 64)
	if err != nil {
		api.Problem(w, r, http.StatusNotFound, "not_found", "no such session", nil)
		return
	}
	s, err := h.store.SessionByID(r.Context(), id)
	if errors.Is(err, sql.ErrNoRows) {
		api.Problem(w, r, http.StatusNotFound, "not_found", "no such session", nil)
		return
	}
	if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
		return
	}
	if s.UserID != a.UserID && a.Role != "admin" {
		api.Problem(w, r, http.StatusForbidden, "forbidden", "not your session", nil)
		return
	}
	if err := h.store.RevokeSession(r.Context(), id); err != nil && !errors.Is(err, sql.ErrNoRows) {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
		return
	}
	api.JSON(w, http.StatusNoContent, nil)
}
