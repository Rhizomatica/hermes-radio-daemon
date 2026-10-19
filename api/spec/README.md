# Pinned API contract

`api-rest.yaml` (REST, OpenAPI 3.1) and `api-ws.yaml` (the `hermes.v1` WebSocket,
AsyncAPI 3.1) are copies of hermes-documentation's `tech_docs/api-redesign/`, the station
API's contract, at commit `6aad461` (2026-10-19). They are
maintained there; update them here only by copying a newer commit, and say which in the
commit message.

The Go router reads `api-rest.yaml` (embedded, `spec.go`): a handler is registered by
`operationId`, and the method, path and role come from the spec, so a renamed path needs no
handler change. `go test ./...` fails if a registered operation or command is not in the
spec.
