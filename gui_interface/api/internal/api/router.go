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
	"net/http"
	"sort"
	"strings"
	"sync"

	"github.com/Rhizomatica/hermes-radio-daemon/gui_interface/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/gui_interface/api/spec"
)

// Router holds the registered operations and commands.
type Router struct {
	mux      *http.ServeMux
	radio    radio.Radio
	ops      map[string]spec.Operation
	commands map[string]command

	// RoleOf gives a request's session role. Until sessions exist
	// (/api/v2/session) every caller is admin: radiod answers /api/v2 only
	// on loopback until then (main:api_v2_enable).
	RoleOf func(r *http.Request) spec.Role

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
		RoleOf:   func(*http.Request) spec.Role { return spec.RoleAdmin },
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
		if !op.Public && !AtLeast(rt.RoleOf(r), op.Role) {
			Problem(w, r, http.StatusForbidden, "forbidden", "this needs the "+string(op.Role)+" role", nil)
			return
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
