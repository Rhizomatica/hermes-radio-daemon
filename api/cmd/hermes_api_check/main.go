// Command hermes_api_check is a self-check for the API layer, not a server.
// It opens the database, builds the station router and reports, as plain
// lines, the health and radio state radiod's C server would serve. Run it as:
//
//	cd api && go run ./cmd/hermes_api_check
//
// It lives outside cmd/hermes_api (the C-archive package) so that radiod never
// links SQLite in: only this command imports internal/store.
package main

import (
	"context"
	"fmt"
	"strings"

	"github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/config"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/station"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
)

func main() { run() }

func run() {
	cfg := config.Load()

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
	rt := station.NewRouter(radio.Default())
	status, _, _, body := rt.ServeRaw("GET", "/api/v2/health", "", "Host: localhost\r\n", "", nil)

	r := rt.Radio()
	state := r.State()

	modem := "not running"
	if _, ok := r.ModemStatus(); ok {
		modem = "ok"
	}

	fmt.Printf("database: %s (%s)\n", dbStatus, cfg.DatabasePath)
	fmt.Printf("health: %d %s\n", status, strings.TrimSpace(string(body)))
	fmt.Printf("radio: %d Hz %s, profile %d, %s\n", state.FrequencyHz, state.Mode, state.ActiveProfile, state.Backend)
	fmt.Printf("modem: %s\n", modem)
	fmt.Printf("version: %s\n", r.Version())
}
