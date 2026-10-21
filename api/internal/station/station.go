// Package station assembles every API domain into one router: the place to
// add a new domain package's Register.
package station

import (
	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/digi"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/gps"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/health"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/modem"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/radio"
)

// NewRouter returns the station's router over a radio, every domain
// registered.
func NewRouter(r rad.Radio) *api.Router {
	rt := api.NewRouter(r)
	health.Register(rt)
	radio.Register(rt)
	digi.Register(rt)
	modem.Register(rt)
	gps.Register(rt)
	return rt
}
