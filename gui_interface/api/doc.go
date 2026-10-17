// Package api is the Go side of hermes-radio-daemon: the station's REST and
// websocket APIs for the UI, linked to the daemon's C core (and mercury's)
// through cgo, the way mercury's gui_interface/fyne-ui links libmercury_core.
//
// Layout, as code arrives:
//
//	cmd/        the Go programs
//	internal/   one package per API domain, named as the REST resources and
//	            websocket topics name them: radio, modem, messages, stations,
//	            system, events, auth, ...
//	cgo/        the seams to the C engines: radio (the daemon's bridge API)
//	            and mercury (libmercury_host)
//
// The module lives here rather than at the repository root so that the C
// sources' vendor/ directory is not taken for Go module vendoring.
package api
