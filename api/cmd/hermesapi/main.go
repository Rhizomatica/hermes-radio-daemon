// Command hermesapi is radiod's Go API layer, built as a C archive
// (go build -buildmode=c-archive -tags radiod): radiod's C HTTP and
// websocket server calls these exports, and the handlers reach the daemon
// through api/radiod_api.h. Nothing here touches a socket.
package main

/*
#cgo CFLAGS: -I${SRCDIR}/../..
#include <stdlib.h>
#include <string.h>
#include "radiod_api.h"
*/
import "C"

import (
	"context"
	"fmt"
	"net/http"
	"os"
	"runtime/debug"
	"strings"
	"time"
	"unsafe"

	"github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/config"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/station"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
)

var router *api.Router

// HermesInit builds the router; radiod calls it once, after attaching the
// C API and before the first request. The Go heap gets a soft limit for
// the 1 GB Pi unless GOMEMLIMIT says otherwise.
//
//export HermesInit
func HermesInit() {
	if os.Getenv("GOMEMLIMIT") == "" {
		debug.SetMemoryLimit(64 << 20)
	}
	router = station.NewRouter(radio.Default())
}

// HermesHTTP answers one request under /api/v2. Every string is C memory,
// read before returning; out's pointers are malloc'd here and freed by C.
//
//export HermesHTTP
func HermesHTTP(method, path, query, headers, peer, body *C.char, bodyLen C.size_t, out *C.radiod_api_http_out) {
	var b []byte
	if bodyLen > 0 {
		b = C.GoBytes(unsafe.Pointer(body), C.int(bodyLen))
	}
	status, ct, extra, resp := router.ServeRaw(C.GoString(method), C.GoString(path), C.GoString(query),
		C.GoString(headers), C.GoString(peer), b)
	out.status = C.int(status)
	out.content_type = C.CString(ct)
	out.extra_headers = C.CString(extra)
	out.body = nil
	out.body_len = 0
	if len(resp) > 0 {
		out.body = (*C.char)(C.CBytes(resp))
		out.body_len = C.size_t(len(resp))
	}
}

// HermesWSOpen checks an upgrade to /api/v2/ws: 0 accepts it (and the
// hello goes out), anything else is the HTTP status to refuse with.
//
//export HermesWSOpen
func HermesWSOpen(conn C.long, path, headers, peer *C.char) C.int {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	r, err := api.RequestFromRaw(ctx, "GET", C.GoString(path), "", C.GoString(headers), C.GoString(peer), nil)
	if err != nil {
		return 400
	}
	return C.int(router.WSOpen(int64(conn), r))
}

// HermesWSMessage takes one text frame of a hermes.v1 connection; it
// returns at once (the command runs on the connection's goroutine).
//
//export HermesWSMessage
func HermesWSMessage(conn C.long, data *C.char, n C.size_t) {
	router.WSMessage(int64(conn), C.GoBytes(unsafe.Pointer(data), C.int(n)))
}

// HermesWSClose ends a hermes.v1 connection.
//
//export HermesWSClose
func HermesWSClose(conn C.long) {
	router.WSClose(int64(conn))
}

// main is `go run ./cmd/hermesapi/main.go`: a self-check, not a server. It
// loads the environment configuration, opens the database, builds the station
// router and logs the health radiod's C server would serve, alongside the
// configured address and the radio state. In radiod, main is never called —
// the daemon's C server drives the Hermes* exports above instead.
func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "hermesapi:", err)
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
