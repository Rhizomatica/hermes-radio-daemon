# api: the station API

radiod's `/api/v2` REST API and `hermes.v1` WebSocket (contract: hermes-documentation
`tech_docs/api-redesign`, pinned in `spec/`). Written in Go and linked into radiod as a C
archive. radiod's C server owns every socket and calls the handlers here; they reach the daemon
only through the radiod C API beside this file, `radiod_api.h`.

```
radiod_api.h/.c   the radiod C API
spec/             the pinned contract and its reader
cmd/hermes_api/    the C exports
cmd/hermes_api_check/  the self-check (go run ./cmd/hermes_api_check); opens the DB, not linked into radiod
cgo/radio/        the Radio interface: the cgo binding (tag radiod) or a stub
internal/api/     the common library: router, problem details, JSON, pagination, hermes.v1
internal/<domain> the handlers, one package per API domain
internal/store/   the SQLite persistence layer (WAL, embedded migrations)
internal/config/  the database path (DATABASE_PATH, default under /var/lib/hermes-radio-daemon)
```

How to add an operation or a command: [docs/API-WRITERS.md](../docs/API-WRITERS.md).
`go test ./...` here needs no C toolchain and no radio.



Here's how to run the Go REST API (`/api/v2`).

## Key point: it's not a standalone Go server

The Go code is **linked into `radio_daemon` as a C archive** (`libhermes_api.a`). radiod's C code owns the socket; the Go handlers only answer requests the C server hands to them. So "running the REST API" means **running radiod with the Go layer linked in and the API enabled**.

## Steps (Debian 13, the station OS)

### 1. Build with Go on the `PATH`

```bash
sudo tests/ci/install-deps.sh   # installs build deps (csdr, libspecbleach, mbelib-neo, ...)
make                            # builds radio_daemon and links the Go API layer
```

`make` detects `go`; if Go isn't present it still builds, but `/api/v2` answers `503 not_supported`.

### 2. Enable the API in `core.ini`

`/etc/hermes/core.ini` (the same file radiod already reads):

```ini
[main]
api_v2_enable = 1
websocket_url = ws://0.0.0.0:8080   ; or wss://0.0.0.0:8443 for TLS
```

- `api_v2_enable = 1` turns the API on (off by default).
- `websocket_url` is the listener. REST is served on the **same** host:port over `http://` (or `https://` when it's `wss://`).

### 3. Run radiod

```bash
./radio_daemon
# or, once installed:
sudo systemctl restart radiod
```

### 4. Hit the endpoints

```bash
curl http://127.0.0.1:8080/api/v2/health
curl http://127.0.0.1:8080/api/v2/radio
curl -X PATCH -H 'Content-Type: application/json' \
     -d '{"frequency_hz":7074000,"mode":"USB"}' \
     http://127.0.0.1:8080/api/v2/radio/profiles/0
```

Currently implemented REST operations (each maps 1:1 to `api/spec/api-rest.yaml`):

| Operation | Request | Role |
|---|---|---|
| `getHealth` | `GET /api/v2/health` | none |
| `getRadioState` | `GET /api/v2/radio` | readonly |
| `listProfiles` | `GET /api/v2/radio/profiles` | readonly |
| `getProfile` | `GET /api/v2/radio/profiles/{profile}` | readonly |
| `updateProfile` | `PATCH /api/v2/radio/profiles/{profile}` | operator |
| `selectProfile` | `POST /api/v2/radio/profiles/{profile}/select` | operator |
| `resetProtection` | `POST /api/v2/radio/protection/reset` | operator |
| `listDigiLog` / `getDigiSettings` / `updateDigiSettings` / `getModemStatus` | digi + modem | various |

## Important: loopback-only for now

Until logins/sessions are implemented:

- `/api/v2` answers **only from localhost** (`127.0.0.1` / `::1`); any other peer gets `403`.
- Every caller is treated as **admin**.

So the curl examples above use `127.0.0.1`.

## Go-only quick checks (no radio, no C)

You don't need radiod to exercise the handlers against a stub:

```bash
cd api
go test ./...                     # all handlers against an in-memory radio stub
go run ./cmd/hermes_api_check      # self-check: opens the DB, builds the router,
                                   # prints database, health, radio, modem and version
```

The self-check prints plain status lines, e.g.:

```
  database: ok (/var/lib/hermes-radio-daemon/hermes.sqlite)
  health: 200 {"components":{"radio":"ok"},"status":"ok"}
  radio: 7100000 Hz USB, profile 0, hamlib
  modem: not running
  version: stub
```

Full test targets: `make go-test test integration-test` (Go unit, C unit, and the real daemon on Hamlib's dummy rig).

Docs: `docs/API-WRITERS.md` (how to add operations/commands) and `docs/ONBOARDING.md` (5-minute intro).