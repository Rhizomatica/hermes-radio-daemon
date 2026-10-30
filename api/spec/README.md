# API contract

`api-rest.yaml` (REST, OpenAPI 3.1) and `api-ws.yaml` (the `hermes.v1` WebSocket,
AsyncAPI 3.1) are the station API's contract, maintained in this repository. They came from
hermes-documentation's `tech_docs/api-redesign/` at commit `6489e6b` (2026-10-21), together with
the design notes and the decision register now in [`docs/api-design/`](../../docs/api-design/).

To change the API, change the spec first, in the same PR as the code or before it. A change to
what the network sees (a path, a field, a status, a role) is a contract change: say so in the PR,
and update `docs/api-design/` when it changes a design decision.

The Go router reads `api-rest.yaml` (embedded, `spec.go`): a handler is registered by
`operationId`, and the method, path and role come from the spec, so a renamed path needs no
handler change. `go test ./...` fails if a registered operation or command is not in the
spec.

`python3 api/spec/gen_reference.py` (needs PyYAML) regenerates `docs/API-REFERENCE.md` and
`docs/API-WS-REFERENCE.md` from these two files and from the operations, commands and topics the Go
code registers, marking each one implemented or planned. Rerun it, and commit the result, whenever you
change the spec or add a handler; the `api-docs` CI job fails if the committed docs differ from a fresh run.
