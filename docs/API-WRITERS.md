# Writing APIs for hermes-radio-daemon

This guide is for people who add to the station's API: the REST API under `/api/v2` and the
`hermes.v1` WebSocket at `/api/v2/ws`. **You write Go only.** radiod's C code owns every
socket (HTTP, HTTPS, WebSocket, and WebRTC to come) and calls your Go handlers; your
handlers reach the radio through a small facade. You never touch a socket, a thread pool or
C memory.

## 1. The contract

What the API looks like is decided in hermes-documentation, not here:

- `tech_docs/api-redesign/api-rest.yaml`: REST, OpenAPI 3.1, every operation with an
  `operationId`;
- `tech_docs/api-redesign/api-ws.yaml`: `hermes.v1`, AsyncAPI 3.1: frames, commands, topics;
- the redesign README's **Security** section: roles on every route, transmit rate limits,
  the audit log, no secrets in URLs or logs, over-the-air text treated as untrusted.

A copy of the two YAML files is pinned in [`gui_interface/api/spec/`](../gui_interface/api/spec/)
with the commit it came from. The router reads it: **you register a handler by
`operationId`, and its method, path and role come from the spec.** A renamed path needs no
code change, and `go test` fails if you register something the spec does not have. To change
the API, change the spec in hermes-documentation first, then re-pin it here.

Conventions you get from the helpers (section 5): JSON in and out, `snake_case`, integer Hz,
ISO 8601 UTC timestamps, RFC 9457 `application/problem+json` errors with a stable string
`code`, `{"data": [...], "next_cursor": "..."}` collections, `X-Request-Id` on every reply.

## 2. How it fits together

```
 browser / app
      |  HTTPS, WebSocket (radiod alone faces the network)
      v
 radiod (C) --- gui_interface/websocket/ws_server.c: sockets, TLS, HTTP parsing
      |            gui_interface/radio_websocket.c: /api/v2 and /api/v2/ws -> Go
      |
      |  C calls the Go exports (cmd/hermesapi): HermesHTTP, HermesWSOpen, ...
      v
 Go API layer (gui_interface/api, linked into radiod as a C archive)
      |  your handlers, internal/<domain>
      |
      |  Go calls the facade (cgo/radio -> gui_interface/radiod_api.h)
      v
 radiod core (C): the radio, DSP, mercury (the modem), the digi log
```

What stays in C, and why: all I/O (sockets, TLS, audio, WebRTC media), DSP and the radio
control loops. They are real-time or security-sensitive, and on a 1 GB Raspberry Pi they
must not pay for a garbage collector. Go builds JSON, validates input and decides; C moves
the bytes.

Legacy interfaces stay as they are, on localhost: radiod's own JSON WebSocket protocol
(`:8080` default, with mercury's UI on it), rigctld (`4532`), CAT (`4534`), mercury's TNC
ports. They are not part of `/api/v2`.

## 3. Directory organisation

```
hermes-radio-daemon/
├── radio_daemon.c          main()
├── radio/                  the radio core: backends, controls, media, pipeline
├── hamlib/  sbitx/         the two backends (Hamlib rigs, the sBitx)
├── dsp/                    modes, filters, digi log
├── modem/                  modem audio paths; mercury_link.c runs mercury inside radiod
├── audioio/                audio bridge, headset
├── control_interfaces/     rigctld, CAT, shared memory (legacy, localhost)
├── gui_interface/
│   ├── websocket/          the HTTP/WebSocket/TLS server (C)
│   ├── radio_websocket.c   radiod's legacy protocol; routes /api/v2 to Go
│   ├── ui_hub.c            module UI traffic (mercury's) for every transport
│   ├── radiod_api.h/.c     THE FACADE: everything Go may read or change
│   ├── web/                the demo web client
│   └── api/                THE GO API LAYER (this guide)
├── common/  include/       config, helpers
├── vendor/                 vendored C libraries; vendor/mercury is a submodule
├── tests/                  C unit tests (Unity), integration tests (Go)
└── docs/                   this guide
```

The Go module, `gui_interface/api` (go.mod: Go 1.27.2):

```
gui_interface/api/
├── spec/               pinned api-rest.yaml, api-ws.yaml, and the reader (spec.go)
├── cmd/hermesapi/      the C exports; converts C types, nothing else
├── cgo/radio/          the Radio interface; facade.go (cgo, build tag `radiod`),
│                       stub.go (an in-memory radio for tests and Go-only work)
└── internal/
    ├── api/            the toolkit: Router, problems, JSON, pagination, hermes.v1
    ├── station/        assembles every domain: add your package's Register here
    ├── testapi/        test helpers: a router over the stub, requests, commands
    ├── health/         getHealth
    ├── radio/          radio state and profiles; radio.set, radio.select_profile, ...
    ├── digi/           digi log and settings
    └── modem/          mercury's status
```

One package per domain, named as the spec's resources and tags name them (`radio`, `digi`,
`modem`, `messages`, `stations`, `system`, ...). A new domain is a new package with a
`Register(rt *api.Router)` function, added to `internal/station/station.go`.

## 4. Working on it

You need Go (the toolchain go.mod names is fetched automatically). You do **not** need a
radio, a C compiler or radiod:

```bash
cd gui_interface/api
go test ./...              # every handler, against the stub radio
go test -race ./...        # what CI runs (make go-test)
```

Without the `radiod` build tag, `radio.Default()` is the stub (`cgo/radio/stub.go`): two
profiles, the same range checks as the real radio, and a record of every frame your code sent.

To run it for real, build radiod with Go on the `PATH` (`make`; it builds
`libhermes_api.a` and links it), and set in `core.ini`:

```ini
[main]
api_v2_enable = 1
```

Then `curl http://127.0.0.1:8080/api/v2/radio`. **Until logins exist, `/api/v2` answers only
to this machine (127.0.0.1, ::1)** and every caller is treated as `admin`; from anywhere else
it answers 403. A radiod built without Go answers `/api/v2` with 503.

## 5. Adding a REST operation

1. **Find the operation in the spec** (`spec/api-rest.yaml`): its `operationId`, request and
   response schemas, `x-hermes-role`. If it is not there, it does not exist yet: propose it to
   hermes-documentation.
2. **Write a handler** in the domain's package: a plain `net/http` handler.
3. **Register it** with `rt.Op(operationId, handler)` in the package's `Register`.
4. **Test it** with `testapi`.

A read, from `internal/radio`:

```go
func (h handlers) getProfile(w http.ResponseWriter, r *http.Request) {
	n, ok := h.profileParam(w, r)                       // {profile} in the spec's path; 404 if none
	if !ok {
		return
	}
	p, err := h.r.Profile(n)
	if err != nil {
		api.Problem(w, r, http.StatusNotFound, "not_found", err.Error(), nil)
		return
	}
	api.JSON(w, http.StatusOK, profileOf(p))           // the spec's RadioProfile shape
}

func Register(rt *api.Router) {
	h := handlers{rt.Radio()}
	rt.Op("getProfile", h.getProfile)                  // GET /api/v2/radio/profiles/{profile}, readonly
}
```

A write is decoded strictly, **validated in full before anything is applied**, and answers
422 with every bad field:

```go
var raw map[string]json.RawMessage
if !api.Decode(w, r, &raw) {            // 415 without application/json, 400 on bad JSON
	return
}
f := api.Fields{}
if power < 0 || power > 100 {
	f.Add("power", "from 0 to 100")
}
if !f.OK() {
	api.Invalid(w, r, f)                // 422 {"code":"invalid","errors":{"power":[...]}}
	return
}
// only now change the radio
```

The toolkit (`internal/api`):

| Call | Does |
| --- | --- |
| `rt.Op(id, h)` | registers by `operationId`; method, path, role from the spec; 403 below the role |
| `api.JSON(w, status, v)` | JSON reply; `v == nil` sends no body (204) |
| `api.Decode(w, r, &dst)` | strict JSON body: `application/json`, no unknown fields |
| `api.Problem(w, r, status, code, detail, errors)` | RFC 9457 problem with `code` and `request_id` |
| `api.Fields`, `api.Invalid(w, r, f)` | per-field validation messages, 422 |
| `api.Limit(w, r)` | `?limit=` (default 50, 1-200) |
| `api.EncodeCursor(n)`, `api.DecodeCursor(w, r)` | opaque cursors |
| `api.Page[T]{Data, NextCursor}` | the collection shape |
| `api.RequestID(r)` | the request's `X-Request-Id` (set on every reply for you) |

Status codes and `code`s used so far: 400 `bad_request`, 403 `forbidden` /
`encryption_not_allowed`, 404 `not_found`, 405 `method_not_allowed`, 409 `busy` (refused while
transmitting), 415 `unsupported_media_type`, 422 `invalid`, 503 `not_running` (mercury absent) /
`not_supported`, 500 `internal`. Use the spec's codes; add a new one to the spec first.

Unknown paths and methods under `/api/v2` are answered for you (404, and 405 with `Allow`). A
handler that panics answers 500 and the daemon keeps running, but don't rely on it.

## 6. Adding a hermes.v1 command

Commands are the WebSocket's `{"type":"cmd","id":"42","cmd":"radio.set","args":{...}}`; each gets
exactly one `{"type":"reply","id":"42","ok":...}`. Register a command the spec's `CommandName`
lists, with the lowest role that may run it:

```go
rt.Command("radio.select_profile", spec.RoleOperator, h.cmdSelect)

func (h handlers) cmdSelect(ctx context.Context, c *api.Conn, args json.RawMessage) (any, *api.CmdError) {
	var a struct {
		Profile *int `json:"profile"`
	}
	if cerr := api.DecodeArgs(args, &a); cerr != nil {   // strict: unknown fields fail
		return nil, cerr
	}
	if a.Profile == nil {
		return nil, api.Errf(api.CodeMissingArg, "profile")
	}
	if *a.Profile < 0 || *a.Profile >= h.r.ProfileCount() {
		return nil, api.Errf(api.CodeOutOfRange, "profile: from 0 to %d", h.r.ProfileCount()-1)
	}
	if err := h.r.SelectProfile(*a.Profile); err != nil {
		return nil, cmdErr(err)                          // radio.ErrBusy -> busy, ...
	}
	return stateOf(h.r.State()), nil                     // the reply's data
}
```

Reply error codes are the spec's list (`api.CodeBadRequest`, `CodeUnknownCmd`,
`CodeMissingArg`, `CodeOutOfRange`, `CodeForbidden`, `CodeBusy`, `CodeNotSupported`, ...). A
REST operation and a command that do the same thing share their validation: `radio.set` and
`PATCH /radio/profiles/{profile}` use one `parsePatch`.

How commands run: radiod hands each frame to Go and returns at once; each connection's
commands run **in order on that connection's own goroutine**, so a slow one (a rig read)
never stalls other clients, and a client sees its replies in the order it sent its commands.
The `ctx` is cancelled after 30 s.

The hello (protocol, version, role, the commands the role may run) and the 5 s heartbeat are
sent for you. **Not there yet:** topics (subscribe, snapshot, events, `seq`), binary spectrum
frames, and WebRTC signalling (`rtc.offer`). They come next; this guide will say how to publish
a topic then.

## 7. Reaching the radio

Handlers get a `radio.Radio` (`rt.Radio()`): state, profiles, setters, digi settings and log,
mercury's status, and sending hermes.v1 frames. Every method is safe from any goroutine and
returns plain Go values; errors are `radio.ErrNoProfile`, `ErrInvalid`, `ErrBusy`,
`ErrNotSupported`.

The setters change the radio exactly as radiod's own commands do, and profile changes are saved
to `user.ini`. Some values are station-wide in radiod (step, SWR threshold, BFO): a profile
reports them all the same.

**If you need something the interface does not have**, it needs a facade call in C
(`gui_interface/radiod_api.h/.c`) and its Go side (`cgo/radio/facade.go`), plus the stub
(`cgo/radio/stub.go`) so tests keep working without C. Ask for it (open an issue or tell the
radiod maintainers what you need and why); keep the facade small and typed, never a way into
radiod's internals.

## 8. Testing

Unit tests, in your package, against the stub (`internal/testapi`):

```go
func TestUpdateProfile(t *testing.T) {
	e := testapi.New(t)                                   // the whole station over a stub radio
	r := e.Do("PATCH", "/radio/profiles/1", `{"power":30}`)
	if r.Status != 200 || r.JSON(t)["power"] != 30.0 {
		t.Fatalf("PATCH = %d %s", r.Status, r.Body)
	}
	e.Radio.SetTX(true)                                   // make the stub transmit
	if r := e.Cmd("radio.select_profile", `{"profile":0}`); r["ok"] != false {
		t.Errorf("selected while transmitting: %v", r)
	}
}
```

Test the refusals as much as the happy path: every 4xx your handler can give, and that a
refused write changed nothing.

Integration tests (`tests/integration/api_v2_test.go`) run the real radiod on Hamlib's dummy
rig and check what crosses the C/Go boundary (a REST write that moves the rig as rigctl sees
it, the hermes.v1 upgrade). Add one when your operation touches the radio in a new way.

```bash
make go-test            # Go unit tests, race detector
make integration-test   # the real daemon
make test               # radiod's C unit tests
```

CI runs all three on Debian 13, amd64 and arm64, with and without the Go layer.

## 9. Rules

- **The contract first.** No operation, field or code that is not in the spec.
- **Validate everything, apply nothing until it is all valid.**
- **Roles come from the spec.** Don't check roles by hand in a handler; transmitting needs
  `operator` (and, once implemented, a rate limit and an audit-log entry).
- **No secrets in URLs or logs.** Passphrases go in bodies or headers.
- **Over-the-air text is untrusted** (decodes, callsigns, chat): pass it through as JSON
  strings, never as markup, and keep the length caps.
- **Threads:** handlers run concurrently on Go's goroutines. Blocking in a handler is fine; it
  blocks only that request. Don't start goroutines that outlive their request or connection.
- **Memory:** the Go heap has a soft 64 MB limit (`GOMEMLIMIT` overrides it) for the 1 GB Pi.
  Page large collections; don't load a whole log.
- **cgo:** only `cgo/radio` talks to C, and only through the facade. Don't add `import "C"`
  elsewhere.

## 10. Not there yet

- **Logins and sessions** (`/api/v2/session`, the database): until then `/api/v2` is
  loopback-only and every caller is `admin`.
- **hermes.v1 topics and binary frames**, then **WebRTC** audio (in C, with libdatachannel; the
  signalling arrives as `rtc.offer` commands).
- **Station-wide radio settings** (`/radio/settings`) and a **transmit timeout**: proposed in
  the spec, not in radiod yet.
- **APIs moving from hermes-api** (messages, users, stations, schedules, ...): each one is a
  domain package written as in this guide.
