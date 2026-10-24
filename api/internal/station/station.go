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
// registered, with no sessions: every caller is admin (radiod answers
// /api/v2 only on loopback until api_v2_enable). The self-check and the
// domain unit tests use this.
func NewRouter(r rad.Radio) *api.Router {
	return newRouter(r, nil)
}

// NewAuthenticatedRouter returns the station's router with sessions and
// users over the store, so RoleOf resolves real logins. radiod uses this.
func NewAuthenticatedRouter(r rad.Radio, st *store.Store) *api.Router {
	return newRouter(r, st)
}

func newRouter(r rad.Radio, st *store.Store) *api.Router {
	rt := api.NewRouter(r)
	health.Register(rt)
	radio.Register(rt)
	digi.Register(rt)
	modem.Register(rt)
	gps.Register(rt)
	if st != nil {
		auth.Register(rt, st, auth.Config{})
		users.Register(rt, st)
		setup.Register(rt, st, setup.Config{})
	}
	return rt
}
