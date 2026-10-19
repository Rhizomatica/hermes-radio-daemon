# ADR — Decision Register

Living document for the Go rewrite (branch `feat/go-rewrite`).

The Node.js/TypeScript backend and its ADRs are preserved for consultation in:

- branch `archive/nodejs-legacy`
- tag `legacy-nodejs-20261015`
- local folder `hermes-backend-backup/`

**Governing rule:** where the source documents differ, Rafael's design is
authoritative — `hermes-documentation/tech_docs/api-redesign/{README.md,
api-rest.yaml, api-ws.yaml, broadcast.md}`. The `hermes-backend/` doc set is a
source of ideas, not a competing contract.

## ADRs

| # | File | Decision | Status |
|---|------|----------|:------:|
| ADR-001 | [adr-001-go-runtime-single-process.md](adr-001-go-runtime-single-process.md) | Go, single binary, one process serves REST+WS; radio/modem via HAL → cgo later | ✅ written |
| ADR-002 | [adr-002-session-token-auth.md](adr-002-session-token-auth.md) | Opaque session tokens (hashed) + argon2id + 4-role RBAC | ✅ written |
| ADR-003 | (planned) | REST `/api/v2` contract — adopts `api-rest.yaml` | pending |
| ADR-004 | (planned) | WebSocket `hermes.v1` — adopts `api-ws.yaml` | pending |
| ADR-005 | (planned) | Broadcast = NNCP area plane, RaptorQ carousel | pending |

## Open decisions (block schema freeze)

| # | Decision | Rafael's position | Owner |
|---|----------|-------------------|-------|
| O-01 | Messaging model | inbox/outbox/drafts + per-recipient delivery (not chat-native) | Product + Backend |
| O-02 | Database engine | MariaDB (deployed default); SQLite open (D-01) | Backend |
| O-03 | Radio config source of truth | DB desired-config + daemon runtime reconciled on boot | Radio + Backend |
| O-04 | Roles for existing users at upgrade | everyone except `root` starts as `user` | Product |
| O-05 | `nncp-transport` branch fate | merge before migration | Maintainer |
| O-06 | Local-only chat (`channel=websocket`) | open | Product + Backend |
