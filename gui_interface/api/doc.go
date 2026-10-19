// Package api is the Go side of hermes-radio-daemon: the station's /api/v2
// REST API and hermes.v1 WebSocket (hermes-documentation tech_docs/api-redesign),
// built as a C archive and linked into radiod. radiod's C code owns every
// socket and calls the handlers here; they reach the daemon through
// gui_interface/radiod_api.h.
//
//	spec/            the pinned contract (api-rest.yaml, api-ws.yaml) and its reader
//	cmd/hermesapi/   the C exports
//	cgo/radio/       the Radio interface: the cgo facade (tag radiod) or a stub
//	internal/api/    the toolkit: router, problems, JSON, pagination, hermes.v1
//	internal/<domain> the handlers, one package per API domain
//
// How to add an operation or a command: docs/API-WRITERS.md.
//
// The module lives here rather than at the repository root so that the C
// sources' vendor/ directory is not taken for Go module vendoring.
package api
