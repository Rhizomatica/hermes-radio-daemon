# Onboarding: the station API in Go

For developers who write the HERMES station API (`/api/v2`, `hermes.v1`) in hermes-radio-daemon.

**The short version:** radiod (hermes-radio-daemon) is the station's backend. Its C code owns every socket: HTTP/HTTPS, WebSocket, and later WebRTC audio. **You write the API in Go only.** Your handlers are linked into radiod and reach the radio only through the radiod C API (`api/radiod_api.h`). You never touch a socket, a thread or C memory.

## Where things are
- **Code:** [`api/`](../api/)
- **Guide (read this first):** [API-WRITERS.md](API-WRITERS.md) covers how it fits together, the layout, adding a REST operation or a hermes.v1 command, testing, and the rules.
- **The contract:** hermes-documentation `tech_docs/api-redesign/`, with `api-rest.yaml` (OpenAPI, REST under `/api/v2`) and `api-ws.yaml` (AsyncAPI, the `hermes.v1` WebSocket at `/api/v2/ws`). Read its README's **Security** section too. A copy is pinned in `api/spec/`. You register handlers **by operationId**, and the method, path and role come from the spec.

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
Until logins exist, `/api/v2` answers only on localhost, and every caller is admin.

## What's there, and what's next
- **Examples to copy** (real operations we need):
  - REST: health, radio state, list/get/update/select profiles, protection reset, digi log (paginated), digi settings, modem (mercury) status;
  - commands: `radio.set`, `radio.select_profile`, `radio.reset_protection`.
  - They work on the real sBitx: we followed a full mercury ARQ call live through `/api/v2/modem`.
- **Coming:**
  - the APIs that move over from hermes-api (messages, users, stations, schedules, ...), one Go package per domain;
  - hermes.v1 topics and events;
  - logins and roles;
  - WebRTC (in C; Go only builds the signalling).

## House rules
1. **Spec first.** No operation, field or error code that isn't in the contract. To change the API, propose it in hermes-documentation first.
2. **Validate everything, apply nothing until all of it is valid.** A refused write changes nothing.
3. **Roles come from the spec;** don't check them by hand. **No secrets in URLs or logs.** **Over-the-air text is untrusted.**
4. **Only `api/cgo/radio` talks to C.** If you need something from radiod that the C API doesn't offer, ask for a new C API function; don't reach into radiod.
5. **Test the refusals** (every 4xx), not only the happy path. Add an integration test when you touch the radio in a new way.
6. **Branch, then a PR to `main`.** CI runs amd64 + arm64, the sanitizers, and a build without Go. It must be green.
7. **Real radios are shared.** Never transmit or restart anything on the bench stations without arranging it with Rafael first.

Questions: Rafael.
