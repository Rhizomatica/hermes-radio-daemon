# Using the HERMES station API

A task-by-task guide to the station's REST API (`/api/v2`) and its WebSocket (`hermes.v1`): log in, log out,
manage users, read and configure GPS, and so on. Every recipe is a `curl` command you can paste.

- **Looking something up?** The complete, generated list of operations, parameters and fields is in
  [API-REFERENCE.md](API-REFERENCE.md) (REST) and [API-WS-REFERENCE.md](API-WS-REFERENCE.md) (WebSocket).
- **Adding to the API?** That is [API-WRITERS.md](API-WRITERS.md).

The sample responses below are real output of the Go layer (captured from the router over its test
radio), so field names and error messages are exact. Values such as timestamps and ids will differ on
your station.

## Contents

1. [Before you start](#1-before-you-start)
2. [First boot: create the first admin](#2-first-boot-create-the-first-admin)
3. [Log in](#3-log-in)
4. [Log out, and manage your sessions](#4-log-out-and-manage-your-sessions)
5. [Manage users](#5-manage-users)
6. [GPS](#6-gps)
7. [Radio, digital modes and modem](#7-radio-digital-modes-and-modem)
8. [Real-time (WebSocket)](#8-real-time-websocket)
9. [Errors and troubleshooting](#9-errors-and-troubleshooting)
10. [What is implemented today](#10-what-is-implemented-today)

---

## 1. Before you start

**Turn the API on.** `/api/v2` is served by radiod itself. Set `api_v2_enable = 1` under `[main]` in
`core.ini` and restart the daemon. Until the C side opens it to the network, `/api/v2` answers **only on
the station itself** (`127.0.0.1`, `::1`); from anywhere else it answers 403. A radiod built without Go
answers 503.

**Set up your shell.** The examples use these variables:

```bash
export BASE=http://127.0.0.1:8080/api/v2     # https://... if the station serves TLS
```

`jq` makes the output easier to read and is used to pull out the token. Add `-sS` to curl to hide the
progress meter but keep errors (the examples already do).

**Roles.** Every account has one role, and each operation needs a minimum role:

| Role | Can |
| --- | --- |
| `readonly` | look at everything a `user` sees; change nothing except their own sessions |
| `user` | the above, plus edit their own profile and password, send messages, log out |
| `operator` | the above, plus control the radio and change radio, digital-mode and GPS settings |
| `admin` | everything, including users, station-wide radio settings and the modem configuration |

A request below the needed role gets **403 `forbidden`**; a request with no valid session gets
**401 `unauthenticated`**. The two are different on purpose: 401 means "log in", 403 means "you can't".

**Things every response has.** An `X-Request-Id` header (send your own, up to 64 characters, and it is
echoed back), and for errors an `application/problem+json` body, described in
[section 9](#9-errors-and-troubleshooting).

---

## 2. First boot: create the first admin

A brand-new station has no users. Anyone on the Wi-Fi could claim it, so setup needs the **one-time setup
code** the installer prints on the console and writes to `/etc/hermes/setup-code` (readable by root only).

**Has setup been done?** (no login needed)

```bash
curl -sS "$BASE/setup"
```
```json
{"done":false}
```

**Run it** (once):

```bash
curl -sS -X POST "$BASE/setup" \
  -H "Content-Type: application/json" \
  -d '{
    "setup_code": "123456",
    "station_name": "Station Alpha",
    "callsign": "PY2XYZ",
    "country": "BR",
    "language": "pt-BR",
    "timezone": "America/Sao_Paulo",
    "admin": { "username": "alice", "password": "a-long-passphrase", "display_name": "Alice" }
  }'
```

You get **201** with the new admin **already logged in** (a `Session`, including its `token`; see
[section 3](#3-log-in)). Required: `setup_code`, `station_name`, `language`, `timezone` and `admin`
(`username`, `password`). `language` is one of `en`, `es`, `pt-BR`, `fr`; `country` is a two-letter
ISO code; the admin's username is 1-32 characters from `a-z 0-9 . _ -`; the password is at least 8.

What can go wrong:

| Status | `code` | Meaning |
| --- | --- | --- |
| 403 | `bad_setup_code` | Wrong code. Five wrong codes lock setup for 10 minutes (429 `rate_limited`, with `Retry-After`). |
| 409 | `setup_done` | Setup already ran. It can only run once. |
| 409 | `conflict` | The admin username is taken. |
| 422 | `invalid` | A field is wrong; `errors` lists each one. |

The setup code is **single use**: the station deletes the code file once setup succeeds.

---

## 3. Log in

Send the username and password to `POST /session`. No session is needed for this call.

```bash
curl -sS -X POST "$BASE/session" \
  -H "Content-Type: application/json" \
  -d '{"username":"alice","password":"a-long-passphrase"}'
```

**201 Created:**

```json
{
  "user": {
    "id": 1, "username": "alice", "display_name": "Alice", "role": "admin",
    "locale": null, "phone": null, "location": null, "disabled": false,
    "last_login_at": "2026-10-29T01:32:04.861Z",
    "created_at": "2026-10-29T01:32:04.778Z", "updated_at": "2026-10-29T01:32:04.778Z"
  },
  "token": "q3J4...",
  "expires_at": "2026-11-05T01:32:04Z"
}
```

The reply carries the session **token** in the body, and also sets it as the `hermes_session` cookie.
A session lasts **7 days**. Use whichever suits your client:

### From a script or tool: Bearer token (simplest)

```bash
TOKEN=$(curl -sS -X POST "$BASE/session" \
  -H "Content-Type: application/json" \
  -d '{"username":"alice","password":"a-long-passphrase"}' | jq -r .token)

curl -sS "$BASE/session" -H "Authorization: Bearer $TOKEN"     # who am I
```

Send `Authorization: Bearer $TOKEN` on every call. Bearer requests need nothing else.

### From a browser: the session cookie

Browsers keep the `hermes_session` cookie (`HttpOnly`, `SameSite=Strict`, and `Secure` when the station
is served over TLS) and send it on their own. Because a browser sends cookies automatically, **requests
that change something** (`POST`, `PUT`, `PATCH`, `DELETE`) made with the cookie must also prove they come
from the station's own page: an `Origin` header naming the station (browsers add it), or
`X-Hermes-CSRF: 1`. Otherwise the answer is **403 `csrf`**. Reads (`GET`) need nothing extra.

You can try the cookie flow with curl's cookie jar:

```bash
curl -sS -c jar.txt -X POST "$BASE/session" \
  -H "Content-Type: application/json" \
  -d '{"username":"alice","password":"a-long-passphrase"}' >/dev/null

curl -sS -b jar.txt "$BASE/session"                                    # read: fine
curl -sS -b jar.txt -X PATCH "$BASE/users/1" \
  -H "X-Hermes-CSRF: 1" -H "Content-Type: application/json" \
  -d '{"display_name":"Alice P."}'                                     # write: needs the header
```

A JavaScript page served by the station can simply `fetch("/api/v2/session", {method: "POST", ...})`;
the browser supplies the cookie and `Origin`.

### From a page on another origin (CORS)

A web app served from somewhere else (another machine on the LAN, or a public site) can call the station
too: by default every origin is allowed (`main:api_cors_origins = *`; a list such as
`https://app.example.org http://192.168.10.5:3000` narrows it, and empty turns CORS off). Such a page
**cannot use the cookie** (the station never allows credentials cross-origin), so it logs in, keeps the
token from the reply, and sends it as a Bearer token:

```js
const base = "http://station.local:8080/api/v2";
const s = await (await fetch(base + "/session", {
  method: "POST", headers: {"Content-Type": "application/json"},
  body: JSON.stringify({username: "alice", password: "a-long-passphrase"}),
})).json();
const radio = await (await fetch(base + "/radio", {
  headers: {Authorization: "Bearer " + s.token},
})).json();
```

Its WebSocket sends the token as a second subprotocol (see section 8).

### Check who is logged in

```bash
curl -sS "$BASE/session" -H "Authorization: Bearer $TOKEN"
```
```json
{"user":{"id":1,"username":"alice","role":"admin", "...":"..."},"expires_at":"2026-11-05T01:32:04Z"}
```

(The token is only returned by the login call, never by this one.)

### When login fails

| Status | `code` | Meaning |
| --- | --- | --- |
| 401 | `unauthenticated` | Wrong username or password. The message is the same whether the user does not exist, the password is wrong, or the account is disabled, so it cannot be used to discover usernames. |
| 429 | `rate_limited` | **5 failed logins a minute per address.** Wait the number of seconds in the `Retry-After` header. A successful login does not count against the limit. |
| 422 / 415 | `invalid` / `unsupported_media_type` | Missing fields, or the body was not `application/json`. |

Once a token stops working (expired, logged out, revoked, or the account was disabled or deleted), calls
return 401 `unauthenticated`: log in again.

---

## 4. Log out, and manage your sessions

**Log out** ends *this* session only. Needs role `user` or above.

```bash
curl -sS -X DELETE "$BASE/session" -H "Authorization: Bearer $TOKEN"     # 204, no body
curl -sS "$BASE/session" -H "Authorization: Bearer $TOKEN"               # now 401
```

(With a cookie, add `-H "X-Hermes-CSRF: 1"` because it is a change.) Logging out of one device does not
affect your sessions on other devices.

**See where you are logged in:**

```bash
curl -sS "$BASE/sessions" -H "Authorization: Bearer $TOKEN"
```
```json
{"data":[{"id":2,"user_id":2,"created_at":"2026-10-29T01:32:04.930Z","last_seen_at":"2026-10-29T01:32:04.930Z",
          "expires_at":"2026-11-05T01:32:04Z","ip":"192.0.2.1","user_agent":null,"current":true}]}
```

`current: true` marks the session making the request. `last_seen_at` is refreshed at most once a minute.

**End another session** (a lost phone, a shared computer): take its `id` from the list.

```bash
curl -sS -X DELETE "$BASE/sessions/2" -H "Authorization: Bearer $TOKEN"  # 204
```

You can end only your own sessions (otherwise 403 `forbidden`, "not your session"). **Admins** can list
and end anyone's: `GET /sessions?user_id=2`, then `DELETE /sessions/{id}`.

Sessions also end on their own when:
- the account is **disabled** or **deleted** (immediately);
- the password is changed: **all other sessions** of that user end, and the one that made the change
  stays (see [change a password](#change-a-password));
- the 7 days run out.

---

## 5. Manage users

All user management uses the token of an **admin** unless stated. In the examples, `$TOKEN` is the admin's.
Usernames are 1-32 characters from `a-z 0-9 . _ -` (lowercase only), and are also the user's mailbox name.

### List users

```bash
curl -sS "$BASE/users?limit=50" -H "Authorization: Bearer $TOKEN"
```
```json
{"data":[{"id":1,"username":"admin","role":"admin", "...":"..."}],"next_cursor":"MQ"}
```

The list is paginated: `limit` is 1-200 (default 50). When `next_cursor` is present there is more; pass it
back as `cursor`. When it is absent, that was the last page.

```bash
curl -sS "$BASE/users?limit=50&cursor=MQ" -H "Authorization: Bearer $TOKEN"
```

### Create a user

```bash
curl -sS -X POST "$BASE/users" \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"username":"maria","password":"s3cret-pass","display_name":"Maria","role":"operator"}'
```

**201** returns the new user. `username` and `password` (at least 8 characters) are required. `role`
defaults to `user`; `display_name`, `locale` (`en`, `es`, `pt-BR`, `fr`), `phone` and `location` are
optional.

| Status | `code` | Meaning |
| --- | --- | --- |
| 409 | `conflict` | Username is taken. |
| 422 | `invalid` | e.g. `{"errors":{"password":["at least 8 characters"],"username":["1 to 32 characters: a-z, 0-9, . _ -"]}}` |

### Look at one user

```bash
curl -sS "$BASE/users/2" -H "Authorization: Bearer $TOKEN"
```

Admins can read any user; everyone else can read **only themselves** (403 for others). 404 if there is no
such user.

### Change a user

`PATCH` with only the fields that change.

```bash
# An admin: change role, or other fields
curl -sS -X PATCH "$BASE/users/2" \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"role":"user","locale":"pt-BR"}'

# Anyone, about themselves: display_name, phone, location only
curl -sS -X PATCH "$BASE/users/2" \
  -H "Authorization: Bearer $MARIA_TOKEN" -H "Content-Type: application/json" \
  -d '{"display_name":"Maria S.","phone":"+55 11 91234-5678"}'
```

Only **admins** may change `role`, `locale` or `disabled`; a user trying gets 403 (`only an admin may
change role, locale or disabled`). Set `phone` or `location` to `null` to clear it.

### Disable a user (reversible) instead of deleting

Disabling blocks the account at once: its sessions stop working (401) and it can no longer log in, but
nothing is lost.

```bash
curl -sS -X PATCH "$BASE/users/2" \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"disabled":true}'
# ...later, to let them back in:
curl -sS -X PATCH "$BASE/users/2" \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"disabled":false}'
```

### Change a password

```bash
# Your own: the current password is required
curl -sS -X PUT "$BASE/users/2/password" \
  -H "Authorization: Bearer $MARIA_TOKEN" -H "Content-Type: application/json" \
  -d '{"current_password":"s3cret-pass","new_password":"a-newer-passphrase"}'     # 204

# An admin resetting someone else's: no current password needed
curl -sS -X PUT "$BASE/users/2/password" \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"new_password":"a-temporary-passphrase"}'                                  # 204
```

`new_password` is at least 8 characters. For your own password, a missing or wrong `current_password` is
a 422 with `errors.current_password` (`required` / `wrong password`). **All the user's other sessions end**
(on a self-change, the session making the call stays; when an admin resets it, all of the user's sessions
end).

### Delete a user

Needs `admin`. Deletes the user and their sessions.

```bash
curl -sS -X DELETE "$BASE/users/2" -H "Authorization: Bearer $TOKEN"     # 204, no body
curl -sS "$BASE/users/2" -H "Authorization: Bearer $TOKEN"               # now 404
```

Things to know before you delete:

- The built-in **`root`** account cannot be deleted (403 `forbidden`).
- A non-admin trying gets 403 (`this needs the admin role`); an unknown id is 404.
- **Their sessions end immediately**: anyone holding their token gets 401.
- **Nothing stops an admin deleting their own account, or the last admin.** If you do, the call succeeds,
  your session ends, and nobody can manage users until another admin is created (setup cannot be re-run).
  Create the replacement admin first.
- If you only want to lock someone out for now, [disable](#disable-a-user-reversible-instead-of-deleting)
  them instead.

Creating and deleting users through the API manages the station's user table. Linking the user's
**mailbox** (Postfix/Dovecot) is done by a station helper that is **not implemented yet**, so for now
creating or deleting a user here does not create or remove a mailbox.

---

## 6. GPS

The station reads its position from the GPS receiver (via gpsd). What the API offers **today**:

| Task | Call | Role |
| --- | --- | --- |
| Read position, fix and settings | `GET /gps` | readonly |
| Change logging and reporting | `PATCH /gps/settings` | operator |
| Watch the position live | the `gps` topic on the [WebSocket](#8-real-time-websocket) | readonly |

### Read the position

```bash
curl -sS "$BASE/gps" -H "Authorization: Bearer $TOKEN"
```
```json
{
  "fix": "none", "latitude": null, "longitude": null, "altitude_m": null, "at": null,
  "settings": {
    "logging": false, "sample_interval_s": 1, "file_period_s": 3600, "email": null,
    "report": { "enabled": false, "interval_s": 900 }
  }
}
```

`fix` is `none` (no position yet; the coordinates are `null`), `2d` (latitude/longitude) or `3d` (also
altitude). `at` is the time of the fix (UTC). Coordinates are decimal degrees, altitude is in metres.

### Change GPS settings

`PATCH /gps/settings` with only what changes (operator or admin). The reply is the settings as they now
are.

```bash
# Record positions, and broadcast this station's position every 10 minutes
curl -sS -X PATCH "$BASE/gps/settings" \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"logging":true,"report":{"enabled":true,"interval_s":600}}'
```
```json
{"logging":true,"sample_interval_s":1,"file_period_s":3600,"email":null,"report":{"enabled":true,"interval_s":600}}
```

| Field | Meaning | Allowed |
| --- | --- | --- |
| `logging` | Record positions to files. | `true` / `false` |
| `sample_interval_s` | Seconds between recorded positions. | integer, 1 or more |
| `file_period_s` | Seconds of positions per file. | integer, 60 or more |
| `email` | Where finished files are sent. | an email address, or `null` to clear |
| `report.enabled` | Broadcast this station's position over the air so other stations can plot it. Off by default. | `true` / `false` |
| `report.interval_s` | Seconds between those broadcasts. | integer, 60 or more |

Everything is checked before anything is applied, and a bad request changes nothing. A 422 lists every
bad field:

```json
{"code":"invalid","status":422,"errors":{"report.interval_s":["from 60 to 2147483647"],
 "sample_interval_s":["from 1 to 2147483647"],"x":["unknown field"]}}
```

### Not available yet

The spec also defines trips (`GET /gps/trips`, `GET /gps/trip`, `POST /gps/trip/close`), the map of other
stations (`GET /gps/stations`), position files (`GET /gps/files`, `DELETE /gps/files`) and an emergency
position message (`POST /gps/sos`). They are **planned**: today they answer **404**. See
[API-REFERENCE.md](API-REFERENCE.md#tag-gps) for their intended shape. The WebSocket `gps` topic currently
carries only the station's own position (`change: "self"`), not other stations' (`change: "peers"`).

---

## 7. Radio, digital modes and modem

Read-only checks and common actions that work today. `$TOKEN` needs the role in the last column.

| Task | Call | Role |
| --- | --- | --- |
| Is the daemon healthy? (no login) | `GET /health` | none |
| Radio state: frequency, mode, PTT, SWR, ... | `GET /radio` | readonly |
| List saved profiles / one profile | `GET /radio/profiles`, `GET /radio/profiles/{n}` | readonly |
| Change a profile (the active one follows at once) | `PATCH /radio/profiles/{n}` | operator |
| Switch to another profile | `PUT /radio/active-profile` | operator |
| Clear the high-SWR protection | `POST /radio/protection/reset` | operator |
| Station-wide radio settings | `GET` / `PATCH /radio/settings` | readonly / admin |
| CW, RTTY, D-STAR settings | `GET` / `PATCH /radio/digi` | readonly / operator |
| Digital-mode traffic log (paginated) | `GET /digi/log` | readonly |
| Modem (Mercury) status | `GET /modem` | readonly |
| Modem configuration | `GET /modem/config` (operator), `PATCH /modem/config` (admin) | |

```bash
curl -sS "$BASE/health"                                                  # no token needed
curl -sS "$BASE/radio" -H "Authorization: Bearer $TOKEN"
curl -sS "$BASE/radio/profiles" -H "Authorization: Bearer $TOKEN"

# Tune the active profile to 7.050 MHz upper sideband (operator)
curl -sS -X PATCH "$BASE/radio/profiles/0" \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" \
  -d '{"frequency_hz":7050000,"mode":"USB"}'

# Switch to profile 1, then clear a SWR trip once the antenna is fixed
curl -sS -X PUT "$BASE/radio/active-profile" \
  -H "Authorization: Bearer $TOKEN" -H "Content-Type: application/json" -d '{"profile":1}'
curl -sS -X POST "$BASE/radio/protection/reset" -H "Authorization: Bearer $TOKEN"
```

Frequencies are integer **Hz**. A `PATCH` is all-or-nothing: if any field is invalid you get 422 listing
them all and nothing changes. The exact fields of each call are in [API-REFERENCE.md](API-REFERENCE.md#tag-radio).
Push-to-talk and sending digital-mode text are not on REST: they are WebSocket commands (`radio.ptt`,
`digi.send`), which are **planned**.

---

## 8. Real-time (WebSocket)

For live state (radio, GPS, modem chat) and commands, use one WebSocket at `/api/v2/ws`, protocol
`hermes.v1`. It uses the same session as REST: a Bearer token for tools, or the cookie for a page served by
the station (the browser must come from the station's own origin, or the upgrade is refused with 403). A
page from another origin can set neither, so it sends the token as a second subprotocol:
`new WebSocket("ws://station.local:8080/api/v2/ws", ["hermes.v1", "hermes.token." + token])`.

```bash
# with websocat (https://github.com/vi/websocat)
websocat --protocol hermes.v1 -H "Authorization: Bearer $TOKEN" ws://127.0.0.1:8080/api/v2/ws
```

```js
// in a page served by the station
const ws = new WebSocket(`wss://${location.host}/api/v2/ws`, "hermes.v1");
ws.onmessage = (e) => console.log(JSON.parse(e.data));
```

**1. The station speaks first**, with `hello`: who you are and what you can use.

```json
{"type":"hello","protocol":"hermes.v1","service":"hermes-radio-daemon","version":"...","role":"admin",
 "topics":["gps","radio.rig","radio.state"],
 "commands":["modem.chat.send","radio.reset_protection","radio.select_profile","radio.set","rig.get","rig.set","rig.watch"],
 "limits":{"max_events_per_s":10,"max_frame_bytes":65536}}
```

`topics` and `commands` are limited to your role, and to what this build implements.

**2. Subscribe** to topics. You get a full snapshot first, then only changes (at most 10 a second per
topic). The `reply` comes after the snapshot.

```json
{"type":"subscribe","id":"1","topics":["gps"]}
```
```json
{"type":"event","topic":"gps","seq":0,"ts":"2026-10-29T01:34:09.600Z","snapshot":true,
 "data":{"change":"self","gps":{"fix":"none","latitude":null,"longitude":null,"altitude_m":null,"at":null,"settings":{"...":"..."}}}}
{"type":"reply","id":"1","ok":true}
```

Leave a topic with `{"type":"unsubscribe","id":"2","topics":["gps"]}`. If `seq` skips a number, subscribe
again to resync.

**3. Run a command.** You choose the `id`; you get exactly one `reply` with the same `id`. Commands need
`operator` (or `user` for chat); `readonly` can only subscribe.

```json
{"type":"cmd","id":"42","cmd":"radio.set","args":{"frequency_hz":7050000,"mode":"USB"}}
```
```json
{"type":"reply","id":"42","ok":true,"data":{"profile":0,"frequency_hz":7050000,"mode":"USB","power":10,"volume":50,"filter_width_hz":null,"digital_voice":false}}
```

An error reply has `"ok":false` and an `error` with a stable `code`:

```json
{"type":"reply","id":"43","ok":false,"error":{"code":"out_of_range","message":"power: from 0 to 100"}}
{"type":"reply","id":"44","ok":false,"error":{"code":"unknown_cmd","message":"no command \"digi.send\""}}
```

The station sends a `heartbeat` (with its clock) every 5 s and drops a client that is silent for 60 s, so
send a ping or any frame at least that often.

Implemented topics: `radio.state`, `radio.rig`, `gps`, `modem.status`, `modem.config`, `modem.chat`.
Implemented commands: `radio.set`, `radio.select_profile`, `radio.reset_protection`, `rig.get`, `rig.set`,
`rig.watch`, `modem.chat.send`. Details and argument tables: [API-WS-REFERENCE.md](API-WS-REFERENCE.md).

---

## 9. Errors and troubleshooting

Every error is `application/problem+json`:

```json
{"type":"https://hermes.radio/problems/forbidden","title":"Forbidden","status":403,"code":"forbidden",
 "detail":"this needs the admin role","request_id":"22d655ff7ecd3646"}
```

Branch on `code` (it is stable); show `detail` to people. Validation failures (422) add `errors`, a list of
messages per field. Quote the `request_id` when reporting a problem: the station's logs use it.

| Status | `code` | Usually means |
| --- | --- | --- |
| 400 | `bad_request` | The body is not valid JSON, or a parameter is malformed. |
| 401 | `unauthenticated` | No session, an expired or revoked token, or a wrong username/password at login. **Log in.** |
| 403 | `forbidden` | Your role is too low for this call, or it is not your resource. The `detail` names the role needed. |
| 403 | `csrf` | A cookie-authenticated write without `Origin` or `X-Hermes-CSRF: 1`. Use a Bearer token, or add the header. |
| 403 | `bad_setup_code` | Wrong first-boot setup code. |
| 404 | `not_found` | No such resource. This also happens for **planned** operations that are in the spec but not implemented yet ([section 10](#10-what-is-implemented-today)). |
| 405 | `method_not_allowed` | Wrong HTTP method for that path (the `Allow` header lists the right ones). |
| 409 | `conflict` / `setup_done` / `busy` | Username taken; setup already done; or the radio is busy (e.g. transmitting). |
| 415 | `unsupported_media_type` | A write without `Content-Type: application/json`. |
| 422 | `invalid` | One or more fields failed validation; see `errors`. Nothing was changed. |
| 429 | `rate_limited` | Too many failed logins (5 a minute) or wrong setup codes (5, then 10 minutes). Wait `Retry-After` seconds. |
| 503 | `not_running` / `not_supported` | The thing behind the call is absent (e.g. the modem is not running). A radiod built without Go answers 503 for all of `/api/v2`. |

**It answers 403 from another computer.** Until the API is opened to the network, `/api/v2` answers only on
the station (`127.0.0.1`). Run the call there (for example over SSH).

**Every call is 401 even though I just logged in.** Check you send `Authorization: Bearer <token>` with
the token from the login *response body*, not the `Set-Cookie` header; and that you did not log out or get
your password changed by an admin (that ends your sessions).

**I log in from a browser over plain HTTP and the cookie vanishes.** The cookie is marked `Secure` only
over TLS, so this works on the station's loopback; if you reach the station through an HTTPS proxy, make
sure the proxy runs on the same machine and sends `X-Forwarded-Proto: https`.

---

## 10. What is implemented today

The contract defines 100 REST operations, 13 WebSocket commands and 15 topics; this build implements the
ones below. Everything else is **planned** and answers 404 (REST) or `unknown_cmd` (WebSocket).

| Area | Implemented |
| --- | --- |
| Health | `getHealth` |
| First boot | `getSetupStatus`, `runSetup` |
| Sessions | `login`, `getSession`, `logout`, `listMySessions`, `revokeSession` |
| Users | `listUsers`, `createUser`, `getUser`, `updateUser`, `deleteUser`, `setPassword` |
| Radio | `getRadioState`, `listProfiles`, `getProfile`, `updateProfile`, `selectProfile`, `resetProtection`, `getRadioSettings`, `updateRadioSettings` |
| Digital modes | `getDigiLog`, `getDigiSettings`, `updateDigiSettings` |
| Modem | `getModemStatus`, `getModemConfig`, `updateModemConfig` |
| GPS | `getGps`, `updateGpsSettings` |

Messages, attachments, stations, schedules, the queue, Wi-Fi, apps, system actions, logs, avatars, devices
and the rest of GPS are in the spec and not built yet.
[API-REFERENCE.md](API-REFERENCE.md) marks every operation **implemented** or **planned**, generated from
the code, so it stays accurate; regenerate it with `python3 api/spec/gen_reference.py` after changing the
spec or adding a handler.
