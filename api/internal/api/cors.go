package api

import (
	"net/http"
	"net/url"
	"strings"
)

// CORS (api-rest.yaml Security): which origins' pages may call /api/v2 from
// elsewhere (main:api_cors_origins). The default, "*", is any origin; a list
// narrows it; empty turns CORS off (same-origin only). Cross-origin calls carry
// a Bearer token: no Access-Control-Allow-Credentials is ever sent, so a
// browser never attaches the session cookie cross-origin and the cookie stays
// same-origin (its SameSite=Strict and the CSRF rule are unchanged). That is
// what makes "*" safe: a page from anywhere can call the API only with a token
// it already holds.

const (
	corsMethods = "GET, HEAD, POST, PUT, PATCH, DELETE"
	corsHeaders = "Authorization, Content-Type, X-Request-Id" // when the preflight names none
	corsExpose  = "X-Request-Id, Retry-After"
	corsMaxAge  = "600"
)

// corsAny is the entry that allows every origin.
const corsAny = "*"

// SetCORSOrigins sets the origins that may call /api/v2 cross-origin, from
// main:api_cors_origins (space or comma separated): "*" for any, or
// scheme://host[:port] entries over http or https. Anything else is left out
// and logged.
func (rt *Router) SetCORSOrigins(list string) {
	rt.cors = map[string]bool{}
	for _, o := range strings.FieldsFunc(list, func(c rune) bool { return c == ',' || c == ' ' || c == '\t' }) {
		if o == corsAny {
			rt.cors[corsAny] = true
		} else if n, ok := normalizeOrigin(o); ok {
			rt.cors[n] = true
		} else {
			rt.Log.Warn("api_cors_origins: not an origin, left out (want scheme://host[:port])", "entry", o)
		}
	}
}

// normalizeOrigin is an origin as browsers send it: lower-case
// scheme://host[:port], nothing after it.
func normalizeOrigin(o string) (string, bool) {
	u, err := url.Parse(strings.TrimSuffix(strings.TrimSpace(o), "/"))
	if err != nil || (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" ||
		u.User != nil || u.Path != "" || u.RawQuery != "" || u.Fragment != "" {
		return "", false
	}
	return strings.ToLower(u.Scheme + "://" + u.Host), true
}

// handleCORS adds the CORS headers for an allowed Origin and answers its
// preflight itself (true: nothing more to do). A preflight carries no
// credentials, so it is answered before the role gate; the request it
// announces then goes through the gate as any other.
//
// Never on the no-session router (Resolve nil): there every loopback caller is
// admin, so CORS would let any page opened in a browser on the station itself
// (a kiosk) run admin operations through 127.0.0.1.
func (rt *Router) handleCORS(w http.ResponseWriter, r *http.Request) bool {
	if len(rt.cors) == 0 || rt.Resolve == nil {
		return false
	}
	w.Header().Add("Vary", "Origin")
	origin := r.Header.Get("Origin")
	if origin == "" {
		return false
	}
	h := w.Header()
	if rt.cors[corsAny] {
		h.Set("Access-Control-Allow-Origin", "*")
	} else if n, ok := normalizeOrigin(origin); ok && rt.cors[n] {
		h.Set("Access-Control-Allow-Origin", origin)
	} else {
		return false
	}
	h.Set("Access-Control-Expose-Headers", corsExpose)
	if r.Method == http.MethodOptions && r.Header.Get("Access-Control-Request-Method") != "" {
		h.Set("Access-Control-Allow-Methods", corsMethods)
		// Whatever headers the page asks for: the role gate, not CORS, is
		// what protects the API.
		if asked := r.Header.Get("Access-Control-Request-Headers"); asked != "" {
			h.Set("Access-Control-Allow-Headers", asked)
		} else {
			h.Set("Access-Control-Allow-Headers", corsHeaders)
		}
		h.Set("Access-Control-Max-Age", corsMaxAge)
		w.WriteHeader(http.StatusNoContent)
		return true
	}
	return false
}

// WSTokenProtocol prefixes a session token sent as a WebSocket subprotocol:
// "Sec-WebSocket-Protocol: hermes.v1, hermes.token.<token>". A browser page
// cannot set Authorization on an upgrade, and the session cookie is
// same-origin only, so this is how a page from another origin opens
// hermes.v1. The station answers with hermes.v1 alone.
const WSTokenProtocol = "hermes.token."

// WSToken is the session token of an upgrade's subprotocol list, or "".
func WSToken(r *http.Request) string {
	for _, p := range strings.Split(r.Header.Get("Sec-WebSocket-Protocol"), ",") {
		if p = strings.TrimSpace(p); strings.HasPrefix(p, WSTokenProtocol) {
			return strings.TrimPrefix(p, WSTokenProtocol)
		}
	}
	return ""
}
