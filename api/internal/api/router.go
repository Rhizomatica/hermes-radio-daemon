// Package api is what API handlers are written with: the router (REST
// operations by operationId, hermes.v1 commands by name), problem+json
// errors, request ids, strict JSON decoding and pagination, following the
// conventions of spec/api-rest.yaml and spec/api-ws.yaml.
package api

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"fmt"
	"log/slog"
	"net/http"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/logging"
	"github.com/Rhizomatica/hermes-radio-daemon/api/spec"
)

// Router holds the registered operations and commands.
type Router struct {
	mux      *http.ServeMux
	radio    radio.Radio
	ops      map[string]spec.Operation
	commands map[string]command
	topics   map[string]topic

	// RoleOf gives a request's session role. Until sessions exist
	// (/api/v2/session) every caller is admin: radiod answers /api/v2 only
	// on loopback until then (main:api_v2_enable).
	RoleOf func(r *http.Request) spec.Role

	// Resolve, when set, turns a request into its principal. The router
	// uses it for the role gate and puts the principal in the request
	// context (PrincipalFrom) before the handler runs. The auth domain
	// installs it; nil means RoleOf is the gate (tests, loopback admin).
	Resolve func(r *http.Request) (Principal, bool)

	// CSRF, when set, reports whether a mutating request passes api-rest.yaml's
	// CSRF rule (the auth domain installs it): a request authenticated with the
	// session cookie must carry an Origin naming this station or
	// X-Hermes-CSRF: 1; Bearer-authenticated requests are exempt.
	CSRF func(r *http.Request) bool

	// Log is the API layer's logger: radiod's (internal/logging), component
	// "api". Requests are logged as "api.http", hermes.v1 as "api.ws".
	Log     *slog.Logger
	httpLog *slog.Logger
	wsLog   *slog.Logger

	conns   sync.Map // int64 -> *Conn (hermes.v1)
	hbStart sync.Once
}

// NewRouter returns an empty router over a radio.
func NewRouter(r radio.Radio) *Router {
	rt := &Router{
		mux:      http.NewServeMux(),
		radio:    r,
		ops:      map[string]spec.Operation{},
		commands: map[string]command{},
		topics:   map[string]topic{},
		RoleOf:   func(*http.Request) spec.Role { return spec.RoleAdmin },
		Log:      logging.New(r, "api"),
		httpLog:  logging.New(r, "api.http"),
		wsLog:    logging.New(r, "api.ws"),
	}
	return rt
}

// Radio is the daemon, for handlers.
func (rt *Router) Radio() radio.Radio { return rt.radio }

// Op registers the handler of one api-rest.yaml operation. Method, path
// and role come from the spec; Op panics on an operationId the spec does
// not have, or one registered twice (so a test of the registration fails
// rather than a request).
func (rt *Router) Op(id string, h http.HandlerFunc) {
	op, err := spec.Op(id)
	if err != nil {
		panic(err)
	}
	if _, dup := rt.ops[id]; dup {
		panic(fmt.Sprintf("api: operation %s registered twice", id))
	}
	rt.ops[id] = op
	rt.mux.HandleFunc(op.Method+" "+spec.BasePath+op.Path, func(w http.ResponseWriter, r *http.Request) {
		if !op.Public {
			role := rt.RoleOf(r)
			if rt.Resolve != nil {
				if p, ok := rt.Resolve(r); ok {
					role = p.Role
					r = WithPrincipal(r, p)
				} else {
					role = spec.RoleNone
				}
			}
			if role == spec.RoleNone {
				// api-rest.yaml: 401 unauthenticated for "not logged in",
				// distinct from a role that is too low.
				Problem(w, r, http.StatusUnauthorized, "unauthenticated", "not logged in, or the session ended", nil)
				return
			}
			if rt.CSRF != nil && r.Method != http.MethodGet && r.Method != http.MethodHead && !rt.CSRF(r) {
				Problem(w, r, http.StatusForbidden, "csrf", "add an Origin header naming this station, or X-Hermes-CSRF: 1", nil)
				return
			}
			if !AtLeast(role, op.Role) {
				Problem(w, r, http.StatusForbidden, "forbidden", "this needs the "+string(op.Role)+" role", nil)
				return
			}
		}
		h(w, r)
	})
}

// Operations lists the registered operationIds, sorted.
func (rt *Router) Operations() []string {
	out := make([]string, 0, len(rt.ops))
	for id := range rt.ops {
		out = append(out, id)
	}
	sort.Strings(out)
	return out
}

var methods = []string{"GET", "POST", "PUT", "PATCH", "DELETE"}

// ServeHTTP answers a request under /api/v2: a request id on every reply,
// and problem+json for unknown paths (404) and methods (405).
func (rt *Router) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	id := requestID(r.Header.Get("X-Request-Id"))
	w.Header().Set("X-Request-Id", id)
	r = r.WithContext(context.WithValue(r.Context(), ctxRequestID{}, id))
	start := time.Now()
	sw := &statusWriter{ResponseWriter: w}
	rt.serve(sw, r)
	rt.logRequest(r, sw.code(), time.Since(start))
}

// statusWriter keeps the status a handler wrote, for the request's log line.
type statusWriter struct {
	http.ResponseWriter
	status int
}

func (w *statusWriter) WriteHeader(status int) {
	if w.status == 0 {
		w.status = status
	}
	w.ResponseWriter.WriteHeader(status)
}

func (w *statusWriter) Write(b []byte) (int, error) {
	if w.status == 0 {
		w.status = http.StatusOK
	}
	return w.ResponseWriter.Write(b)
}

func (w *statusWriter) code() int {
	if w.status == 0 {
		return http.StatusOK
	}
	return w.status
}

// logRequest writes one line per request: debug for reads that worked, info
// for writes and refusals, error for the station's own failures.
func (rt *Router) logRequest(r *http.Request, status int, took time.Duration) {
	level := slog.LevelInfo
	switch {
	case status >= 500:
		level = slog.LevelError
	case status < 400 && (r.Method == http.MethodGet || r.Method == http.MethodHead):
		level = slog.LevelDebug
	}
	target := r.URL.Path
	if r.URL.RawQuery != "" {
		target += "?" + r.URL.RawQuery
	}
	rt.httpLog.Log(r.Context(), level, fmt.Sprintf("%s %s %d", r.Method, target, status),
		"ms", float64(took.Microseconds())/1000, "request_id", RequestID(r))
}

func (rt *Router) serve(w http.ResponseWriter, r *http.Request) {
	if _, pattern := rt.mux.Handler(r); pattern == "" {
		var allow []string
		for _, m := range methods {
			probe := r.Clone(r.Context())
			probe.Method = m
			if _, p := rt.mux.Handler(probe); p != "" {
				allow = append(allow, m)
			}
		}
		if len(allow) > 0 {
			w.Header().Set("Allow", strings.Join(allow, ", "))
			Problem(w, r, http.StatusMethodNotAllowed, "method_not_allowed", r.Method+" is not allowed here", nil)
		} else {
			Problem(w, r, http.StatusNotFound, "not_found", "no such resource: "+r.URL.Path, nil)
		}
		return
	}
	rt.mux.ServeHTTP(w, r)
}

// Principal is the authenticated identity of a request.
type Principal struct {
	SessionID int64
	UserID    int64
	Role      spec.Role
}

type ctxPrincipalKey struct{}

// WithPrincipal returns a copy of r carrying p.
func WithPrincipal(r *http.Request, p Principal) *http.Request {
	return r.WithContext(context.WithValue(r.Context(), ctxPrincipalKey{}, p))
}

// PrincipalFrom returns the request's principal and whether it is set.
func PrincipalFrom(r *http.Request) (Principal, bool) {
	p, ok := r.Context().Value(ctxPrincipalKey{}).(Principal)
	return p, ok
}

type ctxRequestID struct{}

// RequestID is the request's X-Request-Id.
func RequestID(r *http.Request) string {
	id, _ := r.Context().Value(ctxRequestID{}).(string)
	return id
}

// requestID echoes a client's id (up to 64 printable ASCII characters) or
// makes one, as api-rest.yaml's conventions say.
func requestID(given string) string {
	if len(given) > 0 && len(given) <= 64 {
		ok := true
		for _, c := range given {
			if c < 0x21 || c > 0x7e {
				ok = false
				break
			}
		}
		if ok {
			return given
		}
	}
	var b [8]byte
	_, _ = rand.Read(b[:])
	return hex.EncodeToString(b[:])
}

var roleRank = map[spec.Role]int{
	spec.RoleReadonly: 1, spec.RoleUser: 2, spec.RoleOperator: 3, spec.RoleAdmin: 4,
}

// AtLeast reports whether role has may do what min allows.
func AtLeast(role, min spec.Role) bool {
	return roleRank[role] >= roleRank[min] && roleRank[role] > 0
}
