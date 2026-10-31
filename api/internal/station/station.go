// Package station assembles every API domain into one router: the place to
// add a new domain package's Register.
package station

import (
	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/auth"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/digi"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/gps"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/health"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/modem"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/setup"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/users"
)

// NewRouter returns the station's router over a radio, every domain
// registered, with no sessions: every caller is admin, so the router answers
// only on loopback. radiod falls back to it when its database cannot be
// opened; the self-check and the domain unit tests use it.
func NewRouter(r rad.Radio) *api.Router {
	return newRouter(r, nil, setup.Config{})
}

// NewAuthenticatedRouter returns the station's router with sessions and
// users over the store, so RoleOf resolves real logins. cfg tunes the
// first-boot setup (e.g. where the installer's one-time code lives);
// radiod uses this.
func NewAuthenticatedRouter(r rad.Radio, st *store.Store, cfg setup.Config) *api.Router {
	return newRouter(r, st, cfg)
}

func newRouter(r rad.Radio, st *store.Store, cfg setup.Config) *api.Router {
	rt := api.NewRouter(r)
	health.Register(rt)
	radio.Register(rt)
	digi.Register(rt)
	modem.Register(rt)
	gps.Register(rt)
	if st != nil {
		auth.Register(rt, st, auth.Config{})
		users.Register(rt, st)
		setup.Register(rt, st, cfg)
	}
	return rt
}
