// Command hermesapi-check is a self-check for the API layer, not a server.
// It loads the environment configuration, opens the database, builds the
// station router and logs the health radiod's C server would serve, alongside
// the configured address and the radio state. Run it as:
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
	"os"
	"strings"

	"github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/config"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/station"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "hermesapi-check:", err)
		os.Exit(1)
	}
}

func run() error {
	cfg, err := config.Load()
	if err != nil {
		return err
	}

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

💡👾 HERMES GO API RUNNING 👾💡
___________________________________________________________________

  Server           : %s
  Port             : %v
  Database         : %s
  Database Status  : %v
  Log Level        : %s
  Modem            : %v
___________________________________________________________________

  Radio Frequency  : %v Hz
  Radio Mode       : %v
  Radio Profile    : %v
  Radio Backend    : %v
___________________________________________________________________

  Health Status    : %v
  Health Body      : %s
  Version          : %s
  Status           : %v
___________________________________________________________________
`,
		cfg.Addr(),
		cfg.Port,
		cfg.DatabasePath,
		dbStatus,
		cfg.LogLevel,
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

	return nil
}
