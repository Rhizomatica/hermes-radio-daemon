// Command hermesapi-check is a self-check for the API layer, not a server.
// It loads radiod's configuration (core.ini), opens the database, builds the
// station router and logs the health radiod's C server would serve, alongside
// radiod's listener and the radio state. Run it as:
//
//	cd api && go run ./cmd/hermesapi-check
//
// It lives outside cmd/hermesapi (the C-archive package) so that radiod never
// links SQLite in: only this command imports internal/store.
package main

import (
	"context"
	"fmt"
	"net/http"
	"strings"

	"github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/config"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/station"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
)

func main() {
	run()
}

func run() {
	cfg := config.Load()

	rt := station.NewRouter(radio.Default())

	// The database is optional at this stage; report it, do not fail on it.
	dbStatus := "ok"
	st, err := store.Open(context.Background(), cfg.DatabasePath)
	if err != nil {
		dbStatus = "unavailable"
	} else {
		defer st.Close()
		if err := st.PingContext(context.Background()); err != nil {
			dbStatus = "unavailable"
		}
	}

	// Ask the router itself, exactly as radiod's C server would, for /health.
	status, _, _, body := rt.ServeRaw("GET", "/api/v2/health", "", "Host: localhost\r\n", "", nil)

	r := rt.Radio()
	state := r.State()

	modem := "not running"
	if _, ok := r.ModemStatus(); ok {
		modem = "ok"
	}

	healthy := "ok"
	if status != http.StatusOK || dbStatus != "ok" {
		healthy = "degraded"
	}

	var banner strings.Builder

	fmt.Fprintf(&banner, `
___________________________________________________________________

HERMES GO API SELF-CHECK
___________________________________________________________________

  api rest         : %s
  api websocket    : %s   (core.ini main:websocket_url)
  api port         : %s
  database         : %s
  database status  : %s
  modem            : %s
___________________________________________________________________

  radio frequency  : %v Hz
  radio mode       : %v
  radio profile    : %v
  radio backend    : %v
___________________________________________________________________

  health status    : %v
  health body      : %s
  version          : %s
  status           : %s
___________________________________________________________________
`,
		cfg.RESTURL(),
		cfg.WSURL(),
		cfg.Port(),
		cfg.DatabasePath,
		dbStatus,
		modem,
		state.FrequencyHz,
		state.Mode,
		state.ActiveProfile,
		state.Backend,
		status,
		strings.TrimSpace(string(body)),
		r.Version(),
		healthy,
	)

	fmt.Println(banner.String())
}
