# Store and forward: UUCP and NNCP behind one layer

> **Proposal, 2026-11-06; its three decisions taken the same day (at the end).** Nothing
> here is implemented yet.

Stations exchange files, commands and email in store-and-forward jobs: a job waits in a
queue until the station calls, or is called by, the station it is for. HERMES stations do
this over **UUCP** or **NNCP**. radiod's API needs it for messages, for the SOS, and for
anything else that sends a file to another station. This document proposes one layer in
radiod that works the same over both, and exposes what only NNCP has.

## What exists today

- **hermes-api picks one transport per station** (`app/Services/RadioTransport.php`,
  `HERMES_TRANSPORT=uucp|nncp`). It builds a shell command for each job:

  | Job | UUCP | NNCP |
  |---|---|---|
  | The stations it can reach | `/etc/uucp/sys` | `/etc/nncp/neigh` |
  | A file to a station | `uucp -r -j -C -d <file> <dest>!~/<name>` | `nncp-file <file> <dest>:<name>` |
  | The queue | `uustat` | `nncp-stat -pkt` |
  | Removing a job | `uustat -k` | `nncp-rm -node <dest>` |
  | Calling now | `uucico -S <dest>` | `hermes-nncp-call <dest>` |
  | Its own name | `/etc/uucp/config` `nodename` | `/etc/nncp-callsign` |

- **Commands at the other station** (UUCP's `uux`, NNCP's `nncp-exec`) are allowed per
  station: UUCP's `commands` in `/etc/uucp/sys`, NNCP's `HF_HANDLERS` in
  `hermes-nncp-mesh`. Both list `rmail crmail dec_sensors dec_message`.
- **Email** goes through Postfix. The installer's transport map hands mail for another
  station to `rmail` over UUCP or NNCP.
- **Incoming files** land in UUCP's public directory or NNCP's incoming directory.
  `iwatch` calls hermes-api's `GET /api/unpack/<file>` for each one.
- **hermes-sensors' SOS** runs `uux -r - gw!dec_sensors -s ...` with the compressed GPS
  log, then `uucico -S gw`. The gateway's `dec_sensors` emails it.
- **Only NNCP** has areas: one packet to every station of a multicast group, relayed
  between them. Over Mercury's broadcast plane a whole area hears it at once
  ([broadcast.md](broadcast.md)).

## The layer

One Go package in radiod's API, `api/internal/transport`, with one interface and a
backend for each transport. Everything that sends to another station goes through it:
messages, the SOS, the queue endpoints, the station list.

| Operation | What it does |
|---|---|
| `Stations()` | The stations this one can reach, with their aliases and the transports that reach each |
| `Self()` | This station's name |
| `SendFile(dest, name, data)` | A file into the other station's inbox |
| `Exec(dest, handler, args, payload)` | Run an allowed handler at the other station, the payload on its standard input |
| `Area(area, ...)` | An NNCP area packet to every station of an area (NNCP only; see "Area messages") |
| `Call(dest)` | Call the station now instead of at the next scheduled call |
| `Queue()`, `Cancel(job)` | The jobs waiting, and removing one |
| `Capabilities(dest)` | What the transport to `dest` can do: areas, end-to-end encryption |

- **No shell.** Each backend runs its programs with an argument list (`exec.Command`),
  never through `sh -c`, so no user input reaches a shell (a goal of the API design).
- **Jobs have an id.** The id goes into the message's delivery state
  (`uucp_job_id` in the spec becomes `job_id`), so the queue endpoints and a message's
  state talk about the same job.
- **What only NNCP has** shows up in `Capabilities`. An operation the transport can't do
  answers `not_supported`, never a silent fallback.

### Which transport reaches a station

**Per station reached**, not per station sending (decision 1). A station listed in
`/etc/nncp/neigh` is reached over NNCP; one listed only in `/etc/uucp/sys`, over UUCP.
A network can then move from UUCP to NNCP one station at a time. Where a station is
listed in both, a setting picks which one wins.

### Area messages

An area message is not a UUCP or NNCP job to each station (decision 3). It is a file,
most likely a tar.gz of the message, sent once on Mercury's broadcast plane, which every
station of the area hears ([broadcast.md](broadcast.md)). radiod puts it in the
broadcast spool like any other file; the transport the stations use doesn't matter.
NNCP's own area packets (`Area`) stay available where NNCP is, as a capability.

## Receiving

Every station runs the same small handlers, allowed on both transports. Each takes its
payload on standard input and hands it to radiod through a drop directory
(`/var/spool/hermes/in/<handler>/`, writable by the `uucp` and `nncp` groups; decision
2). The handler stays small, and a job that arrives while radiod is down waits there.

| Handler | Carries | radiod does |
|---|---|---|
| `rmail` | email | nothing: Postfix delivers it, as today |
| `dec_message` | a HERMES message (`.hmp`) | files it as a received message |
| `dec_beacon` (new) | a position beacon, the compact layout (14 bytes, [BEACON-FORMAT.md](../BEACON-FORMAT.md)) | the same as a beacon heard on the air: the station list, the map, the digital-message log |
| `dec_sensors` | hermes-sensors' payload | stays with hermes-sensors |

Incoming files (`SendFile`) land in the transports' own directories. radiod watches both
and replaces `iwatch` and `/api/unpack`.

## One message format

A message is the same file whichever way it goes: the HERMES message pack (`.hmp`, what
hermes-api's `dec_message` and `/api/unpack` read today), over UUCP to one station, over
NNCP to one station or in an NNCP bundle to an area, and as the tar.gz on the broadcast
plane. Its definition can be tidied and made smaller in the process (to be written up
separately, with the deployed stations' `.hmp` still read).

## The SOS, the first user

- `POST /gps/sos` sets the emergency code. The beacon carries it on the air, as it does
  now.
- radiod also sends the compact beacon with `Exec(dest, "dec_beacon", payload)` to each
  station of `sos_recipients`, then `Call(dest)`, so the alarm leaves at once, over
  whichever transport reaches that station.
- `DELETE /gps/sos` does the same with code 31, emergency over.
- `sos_recipients` moves from the station settings to the beacon settings
  (`GET`/`PATCH /gps/beacon`, `core.ini` `sos_recipients`), with the rest of what the SOS
  uses. Each name must be a station `Stations()` lists.
- The compact layout fits DATAC16, the most robust mode, so the same 14 bytes serve the
  air and the queue.

## Email

Email stays with Postfix and its transport map, which the installer writes for UUCP or
NNCP. The layer uses `Exec(dest, "rmail", ...)` only for mail radiod itself sends, such
as notifications.

## Who calls whom on HF

Every exchange over the air is one ARQ session: one station calls, the other answers,
and the channel is busy until it ends. On a shared frequency the stations have to take
turns. This is how they do it today, what goes wrong, and what radiod could do instead.

### Today

- **UUCP networks** are a star, and only the gateway calls. hermes-net's `caller.sh`:
  - calls the central server every 315 s, over the TCP uplink;
  - during the time windows set in the GUI (`/api/caller`), calls each remote in turn,
    20 s apart, first retuning to that station's frequency if one is set
    (`/api/frequency`, the GUI's station list).
  - A remote only answers (`uucpd`). It calls only when someone presses "call now"
    (hermes-api, or radiod's `POST /queue/call`). Mail from one remote to another goes
    through the gateway, so it waits up to two rounds.
- **NNCP networks** are a mesh. Every station, the gateway included, calls each neighbour
  it has traffic for, twice an hour, at a minute derived from its callsign
  (`hermes-nncp-mesh`, `NNCP_CALL_CRON`).
- **Mercury** holds a CALL only while it is decoding a frame, for at most one frame and
  its decode, and then keys anyway. It also has a busy detector (VARA's `BUSY ON` /
  `BUSY OFF`), but `uucico`, `nncp-call` and radiod don't wait on it.

### What goes wrong

1. **Calls don't wait for a free channel.** A station that can't hear one side of a
   session calls on top of it. NNCP's spread of call minutes doesn't prevent this: a
   session on HF lasts minutes, and 12 stations calling twice an hour share 30 minutes.
2. **The gateway calls every station, traffic or not.** A call costs 30–60 s of airtime
   before anything moves, so a round of 11 remotes takes 10 minutes or more. A remote
   with mail waits for its turn.
3. **With no time window enabled in the GUI, the gateway never calls a remote.** Their
   mail never leaves.
4. **Three callers that don't know about each other:** `caller.sh`, NNCP's cron and a
   "call now". A "call now" can start in the middle of the gateway's session with
   another station.

### What to do about it

The design is in [hf-channel-access.md](hf-channel-access.md), a separate proposal:
- only the gateway places ARQ calls;
- a remote with traffic asks for one, with a short request frame in a window after the
  gateway's poll frame, and retries a set number of times;
- every periodic frame gets random jitter per station.

## Order of work

1. radiod: `api/internal/transport` with both backends, and the queue and station
   endpoints on it. Tested against fake `uux`, `uucp`, `nncp-exec` and `nncp-file`
   programs on the test's `PATH`.
2. radiod: `dec_beacon` and the drop-directory intake; the SOS over the layer.
3. hermes-installer (private): `dec_beacon` in UUCP's `commands` and NNCP's
   `HF_HANDLERS`, and the drop directories with their groups.
4. radiod: messages (`/messages/{id}/send`) on the layer, and the incoming watch that
   replaces `iwatch`.

## Decisions (2026-11-06)

1. **Which transport reaches a station:** per station reached, not one for the whole
   station.
2. **Handlers hand their payload to radiod** through one drop directory per handler
   under `/var/spool/hermes/in/`, not a local socket.
3. **Area messages** are a file (a tar.gz) on the broadcast plane, not UUCP or NNCP jobs
   to each station.
