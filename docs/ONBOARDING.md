# Onboarding: the station API in Go

For developers who write the HERMES station API (`/api/v2`, `hermes.v1`) in hermes-radio-daemon.

**The short version:** radiod (hermes-radio-daemon) is the station's backend. Its C code owns every socket: HTTP/HTTPS, WebSocket, and later WebRTC audio. **You write the API in Go only.** Your handlers are linked into radiod and reach the radio only through the radiod C API (`api/radiod_api.h`). You never touch a socket, a thread or C memory.

## Where things are
- **Code:** [`api/`](../api/)
- **Guide (read this first):** [API-WRITERS.md](API-WRITERS.md) covers how it fits together, the layout, adding a REST operation or a hermes.v1 command, testing, and the rules.
- **Using the API (log in, users, GPS, ...):** [API-USAGE.md](API-USAGE.md) is a task-by-task how-to; [API-REFERENCE.md](API-REFERENCE.md) and [API-WS-REFERENCE.md](API-WS-REFERENCE.md) list every operation, parameter and field (generated from the spec).
- **The contract:** [`api/spec/`](../api/spec/), with `api-rest.yaml` (OpenAPI, REST under `/api/v2`) and `api-ws.yaml` (AsyncAPI, the `hermes.v1` WebSocket at `/api/v2/ws`). Read the **Security** section of the design notes, [api-design/README.md](api-design/README.md), too. You register handlers **by operationId**, and the method, path and role come from the spec.

## Up and running in 5 minutes (no radio, no C)
```bash
git clone --recursive https://github.com/Rhizomatica/hermes-radio-daemon
cd hermes-radio-daemon/api
go test ./...          # Go 1.27.2; the toolchain is fetched automatically
```
That runs every handler against an in-memory radio stub.

## The whole daemon (Debian 13)
```bash
sudo tests/ci/install-deps.sh      # what CI installs: adds the HERMES APT repo (csdr),
                                   # builds libspecbleach + mbelib-neo into /usr/local
make                               # links the Go layer into radio_daemon
make go-test test integration-test # Go, C, and the real daemon on Hamlib's dummy rig
```
To try it, set `[main] api_v2_enable = 1` in core.ini, run the daemon, then:
`curl http://127.0.0.1:8080/api/v2/radio` (https if the station runs wss).
Sessions exist now (`POST /api/v2/session`, first-boot `/setup`); with `api_v2_enable = 1`,
`/api/v2` answers any client, and the session role gates each operation. If the session
store cannot be opened it still answers only on localhost.

## What's there, and what's next
- **Examples to copy** (real operations we need):
  - REST: health, radio state, list/get/update/select profiles, protection reset, digi log (paginated), digi settings, modem (mercury) status;
  - commands: `radio.set`, `radio.select_profile`, `radio.reset_protection`.
  - authentication: sessions (`internal/auth`), first-boot setup (`internal/setup`), users (`internal/users`), all over `internal/store`.
- **Coming:**
  - the APIs that move over from hermes-api (messages, stations, schedules, ...), one Go package per domain;
  - hermes.v1 topics and events;
  - WebRTC (in C; Go only builds the signalling).

## House rules
1. **Spec first.** No operation, field or error code that isn't in the contract. To change the API, change the spec in `api/spec/` first, in the same PR or before it.
2. **Validate everything, apply nothing until all of it is valid.** A refused write changes nothing.
3. **Roles come from the spec;** don't check them by hand. **No secrets in URLs or logs.** **Over-the-air text is untrusted.**
4. **Only `api/cgo/radio` talks to C.** If you need something from radiod that the C API doesn't offer, ask for a new C API function; don't reach into radiod.
5. **Test the refusals** (every 4xx), not only the happy path. Add an integration test when you touch the radio in a new way.
6. **Branch, then a PR to `main`.** CI runs amd64 + arm64, the sanitizers, and a build without Go. It must be green.

