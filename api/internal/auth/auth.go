// Package auth serves the session operations (api-rest.yaml, tag session)
// and installs the router's RoleOf: a request's role comes from its opaque
// session token, sent as the hermes_session cookie or Authorization: Bearer.
//
// Tokens are 32 random bytes, stored only as a SHA-256 hash; passwords are
// Argon2id (internal/passwd). Revocation is a row delete.
package auth

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/passwd"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/ratelimit"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/users"
	"github.com/Rhizomatica/hermes-radio-daemon/api/spec"
)

// DefaultLifetime is how long a session lives before it must log in again.
// api-rest.yaml's settings.idle_logout_s will replace it once settings land.
const DefaultLifetime = 7 * 24 * time.Hour

// Config tunes Register.
type Config struct {
	// SessionLifetime is how long a new session lasts; <= 0 means the default.
	SessionLifetime time.Duration
}

type handlers struct {
	rt           *api.Router
	store        *store.Store
	lifetime     time.Duration
	loginLimiter *ratelimit.Limiter // failed logins, 5 a minute per address
}

// Register adds the session operations and makes RoleOf resolve sessions.
func Register(rt *api.Router, st *store.Store, cfg Config) {
	if cfg.SessionLifetime <= 0 {
		cfg.SessionLifetime = DefaultLifetime
	}
	h := &handlers{
		rt:           rt,
		store:        st,
		lifetime:     cfg.SessionLifetime,
		loginLimiter: &ratelimit.Limiter{},
	}
	rt.RoleOf = h.roleOf
	rt.Resolve = h.resolvePrincipal
	rt.CSRF = h.csrfOK
	rt.Op("login", h.login)
	rt.Op("getSession", h.getSession)
	rt.Op("logout", h.logout)
	rt.Op("listMySessions", h.listMySessions)
	rt.Op("revokeSession", h.revokeSession)
}

// newToken makes an opaque session token and its SHA-256 hash.
func newToken() (raw, hash string, err error) {
	var b [32]byte
	if _, err := rand.Read(b[:]); err != nil {
		return "", "", err
	}
	raw = base64.RawURLEncoding.EncodeToString(b[:])
	sum := sha256.Sum256([]byte(raw))
	return raw, hex.EncodeToString(sum[:]), nil
}

func hashToken(raw string) string {
	sum := sha256.Sum256([]byte(raw))
	return hex.EncodeToString(sum[:])
}

// tokenFrom reads the session token: the cookie, or Authorization: Bearer.
func tokenFrom(r *http.Request) string {
	if c, err := r.Cookie("hermes_session"); err == nil && c.Value != "" {
		return c.Value
	}
	if a := r.Header.Get("Authorization"); strings.HasPrefix(a, "Bearer ") {
		return strings.TrimPrefix(a, "Bearer ")
	}
	return ""
}

// csrfOK is the router's CSRF check (api-rest.yaml Security): a request that
// changes anything, authenticated with the session cookie, must carry an
// Origin naming this station or X-Hermes-CSRF: 1. Bearer-authenticated
// requests are exempt (browsers never add that header by themselves).
func (h *handlers) csrfOK(r *http.Request) bool {
	c, err := r.Cookie("hermes_session")
	if err != nil || c.Value == "" {
		return true // Bearer or none; the role gate handles "none"
	}
	if r.Header.Get("X-Hermes-CSRF") == "1" {
		return true
	}
	return api.SameOrigin(r)
}

// clientIP is the request's address without its port, for rate limiting.
func clientIP(r *http.Request) string {
	if host, _, err := net.SplitHostPort(r.RemoteAddr); err == nil {
		return host
	}
	return r.RemoteAddr
}

func expired(expiresAt string) bool {
	t, err := time.Parse(time.RFC3339, expiresAt)
	return err != nil || time.Now().After(t)
}

// resolve looks up the request's session, joined with its user's role.
func (h *handlers) resolve(r *http.Request) (store.AuthSession, bool) {
	raw := tokenFrom(r)
	if raw == "" {
		return store.AuthSession{}, false
	}
	a, err := h.store.AuthSessionByTokenHash(r.Context(), hashToken(raw))
	if err != nil || a.Disabled || expired(a.ExpiresAt) {
		return store.AuthSession{}, false
	}
	return a, true
}

func (h *handlers) roleOf(r *http.Request) spec.Role {
	if p, ok := h.resolvePrincipal(r); ok {
		return p.Role
	}
	return spec.RoleNone
}

// resolvePrincipal is the router's Resolve: the request's principal, for
// the role gate and for handlers that need the caller's id and role.
func (h *handlers) resolvePrincipal(r *http.Request) (api.Principal, bool) {
	a, ok := h.resolve(r)
	if !ok {
		return api.Principal{}, false
	}
	return api.Principal{SessionID: a.SessionID, UserID: a.UserID, Role: spec.Role(a.Role)}, true
}

// dummyVerify burns the same Argon2id work on a login for an unknown user,
// so the response time does not reveal which usernames exist.
var dummy struct {
	sync.Once
	hash string
}

func (h *handlers) dummyVerify(password string) {
	dummy.Once.Do(func() {
		if hsh, err := passwd.Hash("hermes-dummy-password"); err == nil {
			dummy.hash = hsh
		}
	})
	if dummy.hash != "" {
		passwd.Verify(dummy.hash, password)
	}
}

// Session is api-rest.yaml's Session.
type Session struct {
	User      users.User `json:"user"`
	Token     string     `json:"token,omitempty"`
	ExpiresAt string     `json:"expires_at"`
}

// sessionInfo is api-rest.yaml's SessionInfo.
type sessionInfo struct {
	ID         int64   `json:"id"`
	UserID     int64   `json:"user_id"`
	CreatedAt  string  `json:"created_at"`
	LastSeenAt string  `json:"last_seen_at"`
	ExpiresAt  string  `json:"expires_at"`
	IP         *string `json:"ip"`
	UserAgent  *string `json:"user_agent"`
	Current    bool    `json:"current"`
}
