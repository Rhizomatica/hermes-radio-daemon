# ADR-001: Go Runtime and Single-Process Station Backend

## Status

**Accepted** (October 2026)

## Governing rule

Where the source documents differ, Rafael's design is authoritative:
`hermes-documentation/tech_docs/api-redesign/{README.md, api-rest.yaml,
api-ws.yaml, broadcast.md}`. This ADR keeps that rule while recording this
repository's host-language choice.

## Supersedes / consolidates

- Legacy ADR-001 — SQLite (WAL) as primary database.
- Legacy ADR-002 — in-process event bus.
- Legacy ADR-006 — Go rewrite and single-process station consolidation.

## Context

### RAM is the binding constraint

The audit (finding CR-1) identifies RAM as the single most critical constraint.
The target is every Raspberry Pi from **1 GB to 16 GB**; the 1 GB Pi sets the
budget. A Node.js process costs ~150–600 MB resident (V8 heap + native modules).
The C radio/modem engines are already accounted for today and do not change.

### The "shared language" rationale is dead

The earlier argument that backend and radio code should share one language was
withdrawn in the legacy ADR-006.

### Go port caveats (port-blocking)

The legacy ADR-006 records two caveats that a Go port must not inherit from the
Node implementation:

- **§R-1 — single-writer serialization.** Node's event loop serialized all writes
  for free; Go does not. The storage layer must re-establish a single writer
  (e.g. one write connection / `SetMaxOpenConns(1)`).
- **§R-2 — event-bus delivery semantics.** `EventEmitter` delivered synchronously
  and isolated listener exceptions; Go channels do not. The delivery model must
  be specified explicitly.

### Relationship to Rafael's Direction

Rafael's Direction (2026-10-09) makes **hermes-radio-daemon the station's
backend** — one process that includes Mercury and serves the REST and WebSocket
APIs, not a separate Go service. This ADR targets the **same end-state** (one
process = backend + radio + modem + REST + WS), with **Go as the host language
for this repository**. The two converge through the cgo consolidation below; the
C/C++ daemon remains the reference implementation.

## Decision

1. **Go, one binary (`hermesd`).** Replaces the Node.js/TypeScript backend.
2. **One process serves both transports** on one port: REST `/api/v2`
   (`api-rest.yaml`) and WebSocket `hermes.v1` (`api-ws.yaml`).
3. **Radio and modem are in-process eventually, behind a HAL now.** An
   `IRadioDriver` interface abstracts the radio; Mercury and hermes-radio-daemon
   remain separate daemons at first and are linked into the process via cgo
   later (legacy ADR-006 spikes S1/S2).
4. **In-process event bus.** A Go pub/sub notifier (buffered channels) with an
   explicit backpressure policy (drop-oldest) and typed topics.
5. **Storage is behind an interface.** The engine is **not settled here**:
   Rafael's design keeps **MariaDB** (already deployed everywhere) and leaves
   **SQLite** as an open question (D-01). SQLite (WAL) remains the leading
   candidate for the 1 GB Pi (RAM, file-copy backups); MariaDB for no-migration
   compatibility. See *Open questions*.
6. **Structured JSON logging** via `log/slog`; configuration from the
   environment only (no global process-env access).

## Consequences

### Positive

- Removes the V8 runtime's resident footprint (measured against a recorded
  baseline, not assumed).
- One artifact to deploy and supervise instead of backend + PHP-FPM + daemons.
- Single-writer, single-process concurrency is simple and auditable.

### Negative

- A Go binary statically linking the DSP/audio libraries is larger on disk than
  a pure-Go binary; disk size is not the win, resident RAM is.
- Single-writer SQLite (if chosen) caps concurrent write throughput; fine at
  2–3 clients, would need rework at many writers.
- cgo consolidation couples the backend's release cycle to the C engines'.

## Alternatives considered

- **Stay on Node.js** — rejected: RAM.
- **Reimplement Mercury and the radio daemon in Go** — rejected: would discard
  hardened real-time DSP and fork upstream.
- **Consolidate into one C/C++ application** — rejected: loses Go's concurrency
  model, stdlib, and ecosystem for the HTTP/WS/DB/auth layers.
- **Link Mercury only, keep radiod as a WebSocket dependency** — viable
  fallback / intermediate step.

## Open questions / validation

1. Database engine (D-01): MariaDB vs SQLite — to be resolved before the schema
   is frozen.
2. Audio stability under load on real sBitx hardware (spikes B/C).
3. Upstream buy-in for `libradio_daemon_core.a` extraction and RT-thread
   self-pinning (`pthread_setaffinity_np`).
4. Kernel isolation (`isolcpus` vs cpuset v2).
5. Transmitter-safety mechanism (systemd watchdog + hardware deadman, separate
   PTT supervisor, or engine-side deadman).
6. Repository/artifact boundary (deferred; see legacy ADR-006).
7. `aarch64` cgo build/CI pipeline and GPL §6 source-offer.
8. Measured RAM vs. the recorded Node + SQLite + engines baseline.

## References

- `hermes-documentation/tech_docs/api-redesign/{README.md, COMPARISON.md, api-rest.yaml, api-ws.yaml}`
- Legacy ADR-001, ADR-002, ADR-006 (branch `archive/nodejs-legacy`)
- `docs/audits/sbitx-v2.md` (carried forward)
