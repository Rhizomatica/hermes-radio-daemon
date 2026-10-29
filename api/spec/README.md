# Pinned API contract

`api-rest.yaml` (REST, OpenAPI 3.1) and `api-ws.yaml` (the `hermes.v1` WebSocket,
AsyncAPI 3.1) are copies of hermes-documentation's `tech_docs/api-redesign/`, the station
API's contract, at commit `6489e6b` (2026-10-21). They are
maintained there; update them here only by copying a newer commit, and say which in the
commit message.

The Go router reads `api-rest.yaml` (embedded, `spec.go`): a handler is registered by
`operationId`, and the method, path and role come from the spec, so a renamed path needs no
handler change. `go test ./...` fails if a registered operation or command is not in the
spec.

`python3 api/spec/gen_reference.py` (needs PyYAML) regenerates `docs/API-REFERENCE.md` and
`docs/API-WS-REFERENCE.md` from these two files and from the operations, commands and topics the Go
code registers, marking each one implemented or planned. Rerun it, and commit the result, whenever you
re-pin the spec or add a handler.
