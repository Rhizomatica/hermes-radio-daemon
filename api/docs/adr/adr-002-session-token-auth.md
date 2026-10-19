# ADR-002: Session-Token Authentication (Opaque Tokens + argon2id)

## Status

**Accepted** (October 2026)

## Supersedes

Legacy ADR-004 — JWT RS256 with refresh-token rotation.

## Context

A station's Wi-Fi is open to its community, so the API must assume anyone can
reach it. Authentication is the REST session: a login issues an opaque token,
and the same token authorizes both REST and the `hermes.v1` WebSocket upgrade.

The legacy backend used JWT RS256 with refresh rotation. The new contract
(`api-rest.yaml` Security section) replaces it with opaque session tokens stored
only as hashes. The legacy ADR-004 also carried a documented claim-set defect
(§R-6): the doc's `permissions`/`station` example did not match the tokens the
implementation actually issued. Opaque tokens remove that class of error.

Passwords move off unsalted SHA-256. Rafael's design specifies **argon2id**.

## Decision

1. **Opaque session tokens.** A 256-bit random token from `crypto/rand`; only its
   **SHA-256 hash** is stored in the `sessions` table. The raw token is never
   persisted.
2. **Two carriers, one session.** The token travels as the `hermes_session`
   cookie (`HttpOnly; Secure; SameSite=Strict`) for browsers, or as
   `Authorization: Bearer <token>` for tools and the WebSocket upgrade.
3. **Password hashing: argon2id** (`golang.org/x/crypto/argon2`), OWASP-recommended
   parameters tuned for the Pi (see *Open questions*).
4. **RBAC with four roles**: `readonly` < `user` < `operator` < `admin`, enforced
   server-side via the spec's `x-hermes-role`. `readonly` sees what `user` sees
   but changes nothing except its own sessions and read marks.
5. **CSRF.** A state-changing request (`POST`/`PUT`/`PATCH`/`DELETE`)
   authenticated by the cookie must carry an `Origin` header naming this station,
   or the `X-Hermes-CSRF: 1` header; otherwise `403 csrf`. `Authorization:
   Bearer` requests are exempt (browsers never send that header unprompted).
6. **Login rate limit.** 5 failures/minute/address → `429 rate_limited`.
7. **First boot.** `/setup` requires the one-time setup code written to the
   console and `/etc/hermes/setup-code` (root only), so no one can claim a new
   station before its owner.
8. **WebSocket reuse.** `GET /api/v2/ws` authenticates with the same
   cookie/Bearer; `Origin` is checked; role gates every command; `readonly` runs
   no commands.
9. **`X-Request-Id`** on every response, echoed if the client sends one; errors
   repeat it as `request_id` for traceability.
10. **Encryption on the air is off by default** (amateur-band rule); an admin may
    enable it in `settings.encryption` for a station licensed to use it.

## Consequences

### Positive

- **Server-side revocation**: deleting the session row logs the token out
  everywhere immediately.
- **No key management** (no RSA keypair, no key rotation).
- **Simplicity**: no refresh-token rotation, no reuse detection, no claim-set
  drift.
- Matches the authoritative specs exactly, so the UI and the backend agree.

### Negative

- **A DB lookup per request** to resolve the session hash → user/role. At 2–3
  concurrent clients this is negligible.
- **Not stateless across stations**: no cross-station token verification. Not
  needed now; federation is out of scope (Phase 10+).

## Alternatives considered

- **Keep JWT RS256** — rejected: the spec mandates opaque session tokens; opaque
  tokens are simpler and allow server-side revocation.
- **HS256** — rejected: shared secret, harder rotation.
- **bcrypt** — rejected: the spec specifies argon2id.
- **scrypt** — considered; argon2id chosen for its memory-hardness and spec
  conformance.

## Open questions

- argon2id exact parameters (memory/iterations/parallelism) — validate on the
  1 GB Pi.
- Session TTL and whether it slides on activity.
- Multiple concurrent sessions per user (allowed, with per-session revocation).

## References

- `spec/api-rest.yaml` — Security section.
- `spec/api-ws.yaml` — Connecting + Security sections.
- Legacy ADR-004 (branch `archive/nodejs-legacy`)
