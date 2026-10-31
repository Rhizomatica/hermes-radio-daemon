package auth

import (
	"database/sql"
	"errors"
	"net"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/passwd"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/users"
	"github.com/Rhizomatica/hermes-radio-daemon/api/spec"
)

// sessionCookie is the Set-Cookie line api-rest.yaml specifies. secure is
// whether the browser may store and send it: only over TLS (or a proxy that
// says so), so the cookie still works on the daemon's plain-HTTP loopback.
func sessionCookie(value string, maxAge int, secure bool) *http.Cookie {
	return &http.Cookie{
		Name: api.SessionCookie, Value: value, Path: "/",
		HttpOnly: true, Secure: secure, SameSite: http.SameSiteStrictMode, MaxAge: maxAge,
	}
}

// secureRequest reports whether the session cookie should be Secure: the
// request came over TLS, or a TLS-terminating proxy on the loopback set
// X-Forwarded-Proto: https. A remote client's X-Forwarded-Proto is not
// trusted (radiod answers /api/v2 only on loopback until api_v2_enable).
func secureRequest(r *http.Request) bool {
	if r.TLS != nil {
		return true
	}
	if !strings.EqualFold(r.Header.Get("X-Forwarded-Proto"), "https") {
		return false
	}
	ip := net.ParseIP(ClientIP(r))
	return ip != nil && ip.IsLoopback()
}

func (h *handlers) login(w http.ResponseWriter, r *http.Request) {
	var req struct {
		Username string `json:"username"`
		Password string `json:"password"`
	}
	if !api.Decode(w, r, &req) {
		return
	}
	// The attempt is counted before the Argon2id work, so parallel guesses
	// cannot all pass the check first; a success (or a server fault) gives
	// the reservation back via its token.
	key := "login:" + RateKey(r)
	ok, retry, token := h.loginLimiter.Reserve(key, 5, time.Minute)
	if !ok {
		w.Header().Set("Retry-After", strconv.Itoa(int(retry.Seconds())+1))
		api.Problem(w, r, http.StatusTooManyRequests, "rate_limited", "too many failed logins", nil)
		return
	}
	u, err := h.store.UserByUsername(r.Context(), req.Username)
	if errors.Is(err, sql.ErrNoRows) {
		// Unknown username: burn the same Argon2id work so the response
		// time does not reveal which usernames exist.
		h.dummyVerify(req.Password)
	} else if err != nil {
		h.loginLimiter.Release(token)
		api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
		return
	} else if passwd.Verify(u.PasswordHash, req.Password) && !u.Disabled {
		// Verify runs before the disabled check, so a disabled account
		// costs the same as a wrong password.
		h.loginLimiter.Release(token)
		h.startSession(w, r, u)
		return
	}
	api.Problem(w, r, http.StatusUnauthorized, "unauthenticated", "wrong username or password", nil)
}

func (h *handlers) startSession(w http.ResponseWriter, r *http.Request, u store.User) {
	h.purgeExpired(r)
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
		UserID: u.ID, TokenHash: hash, IP: ClientIP(r), UserAgent: r.UserAgent(), ExpiresAt: expiresAt,
	}); err != nil {
		return "", "", err
	}
	_ = st.SetLastLoginAt(r.Context(), u.ID)
	http.SetCookie(w, sessionCookie(raw, int(lifetime.Seconds()), secureRequest(r)))
	return raw, expiresAt, nil
}

func (h *handlers) getSession(w http.ResponseWriter, r *http.Request) {
	p, ok := api.PrincipalFrom(r)
	if !ok {
		api.Problem(w, r, http.StatusUnauthorized, "unauthenticated", "not logged in", nil)
		return
	}
	u, err := h.store.UserByID(r.Context(), p.UserID)
	if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
		return
	}
	api.JSON(w, http.StatusOK, Session{User: users.Of(u), ExpiresAt: p.ExpiresAt})
}

func (h *handlers) logout(w http.ResponseWriter, r *http.Request) {
	p, ok := api.PrincipalFrom(r)
	if !ok {
		api.Problem(w, r, http.StatusUnauthorized, "unauthenticated", "not logged in", nil)
		return
	}
	if err := h.store.RevokeSession(r.Context(), p.SessionID); err != nil && !errors.Is(err, sql.ErrNoRows) {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
		return
	}
	http.SetCookie(w, sessionCookie("", -1, secureRequest(r)))
	api.JSON(w, http.StatusNoContent, nil)
}

func (h *handlers) listMySessions(w http.ResponseWriter, r *http.Request) {
	p, ok := api.PrincipalFrom(r)
	if !ok {
		api.Problem(w, r, http.StatusUnauthorized, "unauthenticated", "not logged in", nil)
		return
	}
	target := p.UserID
	if q := r.URL.Query().Get("user_id"); q != "" {
		if p.Role != spec.RoleAdmin {
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
			ExpiresAt: s.ExpiresAt, IP: s.IP, UserAgent: s.UserAgent, Current: s.ID == p.SessionID,
		})
	}
	api.JSON(w, http.StatusOK, map[string]any{"data": out})
}

func (h *handlers) revokeSession(w http.ResponseWriter, r *http.Request) {
	p, ok := api.PrincipalFrom(r)
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
	if s.UserID != p.UserID && p.Role != spec.RoleAdmin {
		api.Problem(w, r, http.StatusForbidden, "forbidden", "not your session", nil)
		return
	}
	if err := h.store.RevokeSession(r.Context(), id); err != nil && !errors.Is(err, sql.ErrNoRows) {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
		return
	}
	api.JSON(w, http.StatusNoContent, nil)
}
