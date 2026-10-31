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
	"sync/atomic"
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
	lastPurge    atomic.Int64       // unix time of the last expired-session purge
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

// tokenFrom reads the session token: the cookie, Authorization: Bearer, or
// (on a WebSocket upgrade from another origin's page) the hermes.token.
// subprotocol. Only the cookie is subject to the Origin and CSRF rules: the
// other two are sent by code that already holds the token.
func tokenFrom(r *http.Request) string {
	if c, err := r.Cookie(api.SessionCookie); err == nil && c.Value != "" {
		return c.Value
	}
	if a := r.Header.Get("Authorization"); strings.HasPrefix(a, "Bearer ") {
		return strings.TrimPrefix(a, "Bearer ")
	}
	return api.WSToken(r)
}

// csrfOK is the router's CSRF check (api-rest.yaml Security): a request that
// changes anything, authenticated with the session cookie, must carry an
// Origin naming this station or X-Hermes-CSRF: 1. Bearer-authenticated
// requests are exempt (browsers never add that header by themselves).
func (h *handlers) csrfOK(r *http.Request) bool {
	if !api.HasSessionCookie(r) {
		return true // Bearer or none; the role gate handles "none"
	}
	if r.Header.Get("X-Hermes-CSRF") == "1" {
		return true
	}
	return api.SameOrigin(r)
}

// ClientIP is the request's address without its port, for the session list.
// radiod's C server hands over the real peer address.
func ClientIP(r *http.Request) string {
	if host, _, err := net.SplitHostPort(r.RemoteAddr); err == nil {
		return host
	}
	return r.RemoteAddr
}

// RateKey is the client's rate-limit bucket: its IPv4 address, or its IPv6
// /64. One IPv6 host commonly owns a whole /64, so a bucket per address would
// let it rotate addresses for fresh attempts.
func RateKey(r *http.Request) string {
	ip := net.ParseIP(ClientIP(r))
	if ip == nil {
		return ClientIP(r)
	}
	if v4 := ip.To4(); v4 != nil {
		return v4.String()
	}
	return ip.Mask(net.CIDRMask(64, 128)).String() + "/64"
}

func expired(expiresAt string) bool {
	t, err := time.Parse(time.RFC3339, expiresAt)
	return err != nil || time.Now().After(t)
}

// lastSeenTouchEvery throttles how often resolve bumps a session's
// last_seen_at, so an authenticated read does not become a write on every
// request.
const lastSeenTouchEvery = time.Minute

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
	if t, err := time.Parse(time.RFC3339, a.LastSeenAt); err != nil || time.Since(t) > lastSeenTouchEvery {
		_ = h.store.TouchSession(r.Context(), a.SessionID) // best-effort
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
	return api.Principal{SessionID: a.SessionID, UserID: a.UserID, Role: spec.Role(a.Role), ExpiresAt: a.ExpiresAt}, true
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

// purgeEvery is how often a login sweeps expired sessions out of the table.
const purgeEvery = time.Hour

// purgeExpired deletes expired sessions, at most once per purgeEvery. It runs
// on login, so an idle station does no work; failures are best-effort.
func (h *handlers) purgeExpired(r *http.Request) {
	now := time.Now()
	last := h.lastPurge.Load()
	if now.Unix()-last < int64(purgeEvery.Seconds()) || !h.lastPurge.CompareAndSwap(last, now.Unix()) {
		return
	}
	_, _ = h.store.PurgeExpiredSessions(r.Context(), now)
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
