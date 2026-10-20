// Command hermesapi is radiod's Go API layer, built as a C archive
// (go build -buildmode=c-archive -tags radiod): radiod's C HTTP and
// websocket server calls these exports, and the handlers reach the daemon
// through api/radiod_api.h. Nothing here touches a socket.
//
// This package must not import internal/store (or anything that pulls SQLite
// in): radiod never opens a database, and linking modernc.org/sqlite would
// bloat radiod for nothing. The standalone self-check lives in
// cmd/hermesapi-check.
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
	"os"
	"runtime/debug"
	"time"
	"unsafe"

	"github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/station"
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

// main exists only because a c-archive must be a main package. radiod's C
// server drives the Hermes* exports above, so main is never called there;
// the standalone self-check lives in cmd/hermesapi-check.
func main() {}
