# api: the station API

radiod's `/api/v2` REST API and `hermes.v1` WebSocket (contract: hermes-documentation
`tech_docs/api-redesign`, pinned in `spec/`). Written in Go and linked into radiod as a C
archive. radiod's C server owns every socket and calls the handlers here; they reach the daemon
only through the radiod C API beside this file, `radiod_api.h`.

```
radiod_api.h/.c   the radiod C API
spec/             the pinned contract and its reader
cmd/hermesapi/    the C exports
cgo/radio/        the Radio interface: the cgo binding (tag radiod) or a stub
internal/api/     the common library: router, problem details, JSON, pagination, hermes.v1
internal/<domain> the handlers, one package per API domain
```

How to add an operation or a command: [docs/API-WRITERS.md](../docs/API-WRITERS.md).
`go test ./...` here needs no C toolchain and no radio.
