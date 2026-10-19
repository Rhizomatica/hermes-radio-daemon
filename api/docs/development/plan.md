# Development Plan (Go rewrite)

Target: single Go binary serving REST `/api/v2` + WebSocket `hermes.v1`, SQLite.
See [`../adr/README.md`](../adr/README.md) for the decision register.

## Phases

| Phase | Scope | Exit gate |
|-------|-------|-----------|
| 1. Skeleton | Go module, config, `slog`, SQLite + migrations, `/health`, graceful shutdown, systemd unit | `go test ./...`, boots, health 200 |
| 2. Auth core | Session store (hashed), login/logout, RBAC (4 roles), CSRF/Origin, rate limit, `X-Request-Id`, `/setup` code, argon2id | Auth conformance tests |
| 3. REST | `api-rest.yaml` domains: users → settings → schedules → stations → messages/attachments → events → radio → email/UUCP → geolocation → reticulum | OpenAPI conformance |
| 4. WebSocket | `hermes.v1`: subscribe/publish/snapshot/`seq`, heartbeat, backpressure, binary audio, role gating, PTT-on-disconnect | WS conformance suite |
| 5. Radio | `IRadioDriver` (simulated + sBitx CLI); cgo consolidation (legacy ADR-006 spikes) | Telemetry + TX-safety tests |
| 6. Compatibility | `.hmp`, UUCP/Postfix/Dovecot ties, frozen legacy `/api` shim, CLI replacements (`iwatch`, `caller.sh`, `mailkill.sh`) | Scripts run without HTTP |
| 7. Hardening | TLS, security headers, observability, `aarch64` cross-build, power-loss/SD-card + RAM-vs-baseline | Full gate suite |
| 8. Docs + release | Docs reflect code; first tag | — |

## Continuous quality gates

`go build ./...` · `go vet ./...` · `golangci-lint run` · `go test -race ./...` ·
fuzz tests for untrusted over-the-air text · conformance suites generated from
the two vendored specs.

## Package layout

```
cmd/hermesd/main.go
internal/
├── config/      # env config
├── server/      # HTTP server, middleware
├── api/         # REST /api/v2 handlers
├── ws/          # hermes.v1 gateway
├── auth/        # sessions, RBAC, argon2id
├── store/       # sqlite + migrations + repositories
├── radio/       # IRadioDriver HAL
├── messaging/   # messages / email / UUCP / .hmp
├── events/      # in-process bus → ws
├── i18n/        # locale resources
└── system/      # health, observability, reboot
```
