#!/usr/bin/env python3
"""Generate docs/API-REFERENCE.md and docs/API-WS-REFERENCE.md from the pinned
contract (api-rest.yaml, api-ws.yaml) and from what the Go code registers.

Run from anywhere:   python3 api/spec/gen_reference.py
Needs PyYAML (a dev-time tool only; the daemon and `go test` do not use it).

What is generated, never hand-edited:
  - every operation, parameter, body and response of the spec;
  - whether each operation / command / topic is IMPLEMENTED (registered in
    api/internal) or only PLANNED (in the spec, no handler yet).
The how-to guide, docs/API-USAGE.md, is written by hand.
"""
import re
import sys
from pathlib import Path

import yaml

HERE = Path(__file__).resolve().parent
API = HERE.parent
DOCS = API.parent / "docs"
METHODS = ("get", "post", "put", "patch", "delete")

rest = yaml.safe_load((HERE / "api-rest.yaml").read_text())
ws = yaml.safe_load((HERE / "api-ws.yaml").read_text())
RS = rest["components"]["schemas"]
WS = ws["components"]["schemas"]


# ── what the Go code implements ────────────────────────────────────────────
def registered(pattern):
    found = set()
    for f in (API / "internal").rglob("*.go"):
        if f.name.endswith("_test.go"):
            continue
        found |= set(re.findall(pattern, f.read_text()))
    return found


IMPL_OPS = registered(r'\.Op\("(\w+)"')
IMPL_CMDS = registered(r'\.Command\("([\w.]+)"')
IMPL_TOPICS = registered(r'\.(?:Topic|TopicConn|Stream|Binary)\("([\w.]+)"')


# ── small helpers ──────────────────────────────────────────────────────────
def one_line(s):
    return re.sub(r"\s+", " ", (s or "").strip()).replace("|", "\\|")


def first_sentence(s):
    s = one_line(s).replace("\\|", "|")
    m = re.match(r"(.+?[.!?])(\s|$)", s)
    return (m.group(1) if m else s).replace("|", "\\|")


def ref_name(ref):
    return ref.rsplit("/", 1)[-1]


def schema_link(name, from_ws=False):
    if from_ws and name in RS:
        return f"[{name}](API-REFERENCE.md#schema-{name.lower()})"
    return f"[{name}](#schema-{name.lower()})"


def type_str(s, from_ws=False):
    """A short, readable type for a (sub)schema."""
    if s is None or s == {}:
        return "any"
    if "$ref" in s:
        name = ref_name(s["$ref"])
        cross = "api-rest.yaml" in s["$ref"]
        return schema_link(name, from_ws=(from_ws and (cross or name in RS and name not in WS)))
    for key in ("oneOf", "anyOf"):
        if key in s:
            return " or ".join(type_str(x, from_ws) for x in s[key])
    if "allOf" in s:
        return " + ".join(type_str(x, from_ws) for x in s["allOf"])
    if "const" in s:
        return f"`{s['const']}`"
    if "enum" in s:
        return "one of " + ", ".join(f"`{v}`" for v in s["enum"])
    t = s.get("type")
    if isinstance(t, list):
        t = " or ".join(x for x in t)
    if t == "array":
        return "array of " + type_str(s.get("items"), from_ws)
    if t == "object":
        if s.get("properties"):
            return "object"
        ap = s.get("additionalProperties")
        if isinstance(ap, dict):
            return "map of " + type_str(ap, from_ws)
        return "object"
    if t is None:
        return "any"
    if s.get("format"):
        t += f" ({s['format']})"
    return t


def constraints(s):
    out = []
    if not isinstance(s, dict):
        return out
    for k, label in (("minimum", "min"), ("maximum", "max"), ("minLength", "min length"),
                     ("maxLength", "max length"), ("minItems", "min items"), ("pattern", "pattern")):
        if k in s:
            v = f"`{s[k]}`" if k == "pattern" else s[k]
            out.append(f"{label} {v}")
    if "default" in s:
        out.append(f"default `{s['default']}`")
    if s.get("readOnly"):
        out.append("read-only")
    if s.get("writeOnly"):
        out.append("write-only")
    return out


def resolve(s, local=None):
    """Follow a $ref to its schema (REST schemas; WS-local ones too)."""
    seen = 0
    while isinstance(s, dict) and "$ref" in s and seen < 10:
        name = ref_name(s["$ref"])
        pool = RS if ("api-rest.yaml" in s["$ref"] or local is None) else local
        s = pool.get(name, RS.get(name, {}))
        seen += 1
    return s


def object_parts(s, local=None):
    """(properties, required) of an object schema, merging allOf."""
    s = resolve(s, local)
    props, req = {}, set()
    for part in s.get("allOf", []):
        p, r = object_parts(part, local)
        props.update(p)
        req |= r
    props.update(s.get("properties", {}) or {})
    req |= set(s.get("required", []) or [])
    return props, req


def field_table(s, from_ws=False, local=None, indent=""):
    props, req = object_parts(s, local)
    if not props:
        return ""
    lines = [f"{indent}| Field | Type | Required | Notes |", f"{indent}| --- | --- | --- | --- |"]
    for name, ps in props.items():
        notes = [one_line(ps.get("description", "")) if isinstance(ps, dict) else ""]
        rs = resolve(ps, local) if isinstance(ps, dict) else {}
        c = constraints(ps) + [x for x in constraints(rs) if x not in constraints(ps)]
        if "$ref" in ps and not notes[0]:
            notes = [first_sentence(rs.get("description", ""))]
        notes = [n.rstrip(".") for n in notes if n] + c
        lines.append(f"{indent}| `{name}` | {type_str(ps, from_ws)} | {'yes' if name in req else ''} | {'; '.join(notes)} |")
    return "\n".join(lines) + "\n"


# Realistic sample values by field name, so generated examples read like real calls.
HINTS = {
    "username": "alice", "password": "a-long-passphrase", "current_password": "old-passphrase",
    "new_password": "new-long-passphrase", "display_name": "Alice", "email": "alice@example.org",
    "phone": "+55 11 91234-5678", "location": "Base camp", "station_name": "Station Alpha",
    "callsign": "PY2XYZ", "country": "BR", "timezone": "America/Sao_Paulo", "setup_code": "123456",
    "text": "CQ CQ de PY2XYZ", "name": "AF", "sdp": "v=0...",
}


def example_value(s, local=None, depth=0, name=None):
    if not isinstance(s, dict):
        return None
    if name in HINTS and "example" not in s and "enum" not in s and "$ref" not in s:
        return HINTS[name]
    if "example" in s:
        return s["example"]
    if s.get("examples"):
        return s["examples"][0]
    if "default" in s:
        return s["default"]
    if "const" in s:
        return s["const"]
    if "enum" in s:
        return s["enum"][0]
    s = resolve(s, local)
    for k in ("oneOf", "anyOf"):
        if k in s:
            return example_value(s[k][0], local, depth)
    if "example" in s:
        return s["example"]
    if s.get("examples"):
        return s["examples"][0]
    if "enum" in s:
        return s["enum"][0]
    t = s.get("type")
    if isinstance(t, list):
        t = next((x for x in t if x != "null"), "string")
    if "allOf" in s or t == "object" or "properties" in s:
        if depth > 3:
            return {}
        props, req = object_parts(s, local)
        keys = [k for k in props if k in req] or list(props)[:1]
        return {k: example_value(props[k], local, depth + 1, k) for k in keys}
    if t == "array":
        return [example_value(s.get("items"), local, depth + 1)]
    if t == "integer":
        return s.get("minimum", 0)
    if t == "number":
        return 0
    if t == "boolean":
        return False
    fmt = s.get("format")
    if fmt == "date-time":
        return "2026-10-09T14:03:00Z"
    if fmt == "email":
        return "user@example.org"
    return "string"


def to_json(v):
    import json
    return json.dumps(v, indent=2, ensure_ascii=False)


# ── REST reference ─────────────────────────────────────────────────────────
def role_of(op):
    if op.get("security") == []:
        return "none (public)"
    return op.get("x-hermes-role", "?")


def param_rows(path_item, op):
    params = list(path_item.get("parameters", [])) + list(op.get("parameters", []))
    rows = []
    for p in params:
        if "$ref" in p:
            p = rest["components"]["parameters"][ref_name(p["$ref"])]
        sch = p.get("schema", {})
        notes = [one_line(p.get("description", ""))] + constraints(sch)
        rows.append(f"| `{p['name']}` | {p['in']} | {type_str(sch)} | {'yes' if p.get('required') else ''} | "
                    f"{'; '.join(n for n in notes if n)} |")
    return rows


# Sample values for path parameters, so the examples can be pasted as they are.
PATH_SAMPLES = {"user_id": "2", "session_id": "42", "profile": "default"}

# What the build does today where the spec text promises more. Shown under an
# implemented operation's spec description, so planned behaviour is not read
# as current.
NOTES = {
    "createUser": "The mailbox is not created yet: the station's `email_create_user` helper is not "
                  "wired in. Only the account is created.",
    "deleteUser": "The mailbox is not removed yet: the station's email helper is not wired in. Only "
                  "the account and its sessions are removed.",
    "setPassword": "The mailbox password is not updated yet: the station's email helper is not "
                   "wired in. Only the login password changes.",
}


def curl_example(method, path, op, path_item):
    url = "$BASE" + re.sub(r"\{(\w+)\}", lambda m: PATH_SAMPLES.get(m.group(1), m.group(0)), path)
    lines = [f"curl -sS -X {method.upper()} \"{url}\""]
    if op.get("security") != []:
        lines.append('  -H "Authorization: Bearer $TOKEN"')
    body = op.get("requestBody")
    if body:
        ct, media = next(iter(body["content"].items()))
        if ct == "application/json":
            sch = media.get("schema", {})
            ex = example_value(sch)
            lines.append('  -H "Content-Type: application/json"')
            lines.append("  -d '" + to_json(ex).replace("'", "'\\''") + "'")
    return " \\\n".join(lines)


def response_rows(op):
    rows = []
    for code, r in op.get("responses", {}).items():
        if "$ref" in r:
            r = rest["components"]["responses"][ref_name(r["$ref"])]
        body = ""
        content = r.get("content", {})
        if content:
            ct, media = next(iter(content.items()))
            body = f"`{ct}` " + type_str(media.get("schema", {}))
        rows.append(f"| {code} | {one_line(r.get('description', ''))} | {body} |")
    return rows


def gen_rest():
    out = []
    w = out.append
    ops_by_tag = {}
    total = impl = 0
    for path, item in rest["paths"].items():
        for m in METHODS:
            if m in item:
                op = item[m]
                tag = (op.get("tags") or ["other"])[0]
                ops_by_tag.setdefault(tag, []).append((path, m, op, item))
                total += 1
                impl += op["operationId"] in IMPL_OPS
    w("# HERMES station REST API reference\n")
    w("<!-- GENERATED by api/spec/gen_reference.py from api/spec/api-rest.yaml. Do not edit by hand. -->\n")
    w(f"The REST API of a HERMES station, served by hermes-radio-daemon under `/api/v2`. "
      f"This page lists **every operation of the contract** ({total}); **{impl} are implemented in this "
      f"build**, the rest are *planned* (in the spec, no handler yet, so they answer 404).\n")
    w("- New to the API? Start with the how-to guide: **[API-USAGE.md](API-USAGE.md)** "
      "(log in, log out, users, GPS, ...).")
    w("- Real-time (WebSocket) API: [API-WS-REFERENCE.md](API-WS-REFERENCE.md).")
    w("- The contract itself: [`api/spec/api-rest.yaml`](../api/spec/api-rest.yaml), "
      f"version `{rest['info']['version']}`.\n")
    w("## Conventions\n")
    w("""- **Base URL:** `http(s)://<station>/api/v2` (in this document, paths are relative to it).
- **Format:** JSON in and out, `snake_case`, integer Hz, ISO 8601 UTC timestamps. Writes send
  `Content-Type: application/json`; unknown fields are refused (422).
- **Authentication:** an opaque session token, from `POST /session`, sent as the `hermes_session`
  cookie (browsers) or `Authorization: Bearer <token>` (tools). No session: 401.
- **Roles**, lowest to highest: `readonly` < `user` < `operator` < `admin`. An operation's *Role* is
  the lowest role that may call it; a lower one gets 403.
- **CSRF:** a request that changes something (`POST`, `PUT`, `PATCH`, `DELETE`) authenticated with the
  *cookie* must carry an `Origin` naming the station, or `X-Hermes-CSRF: 1` (403 `csrf` otherwise).
  Bearer requests are exempt.
- **Collections** return `{"data": [...], "next_cursor": "..."}` and take `limit` (default 50, max 200)
  and `cursor`; `next_cursor` is absent on the last page.
- **Create** returns 201 with the object, **update** is `PATCH` with only the fields that change,
  **delete** returns 204. No `GET` changes anything.
- **Every response** has an `X-Request-Id` (send your own, up to 64 characters, and it is echoed).
- **Errors** are `application/problem+json` ([RFC 9457](https://www.rfc-editor.org/rfc/rfc9457)) with a
  stable string `code`, a `detail`, the `request_id`, and for validation failures `errors` per field.
""")
    w("### Standard errors\n")
    w("| Status | Name | Meaning |\n| --- | --- | --- |")
    codes = {"Unauthorized": 401, "Forbidden": 403, "NotFound": 404, "Conflict": 409, "Invalid": 422,
             "TooLarge": 413, "TooManyRequests": 429}
    for name, r in sorted(rest["components"]["responses"].items(), key=lambda kv: codes.get(kv[0], 999)):
        w(f"| {codes.get(name, '')} | {name} | {one_line(r.get('description', ''))} |")
    w("")
    w("## Contents\n")
    tags = {t["name"]: t for t in rest.get("tags", [])}
    for tag, items in ops_by_tag.items():
        n = sum(o["operationId"] in IMPL_OPS for _, _, o, _ in items)
        w(f"- [{tag}](#tag-{tag}): {len(items)} operations, {n} implemented")
    w("")
    for tag, items in ops_by_tag.items():
        w(f"<a id=\"tag-{tag}\"></a>\n## {tag}\n")
        w(one_line(tags.get(tag, {}).get("description", "")) + "\n")
        w("| Operation | Method and path | Role | Status |\n| --- | --- | --- | --- |")
        for path, m, op, _ in items:
            ok = "implemented" if op["operationId"] in IMPL_OPS else "planned"
            w(f"| [`{op['operationId']}`](#op-{op['operationId'].lower()}) | `{m.upper()} {path}` | "
              f"{role_of(op)} | {ok} |")
        w("")
        for path, m, op, item in items:
            oid = op["operationId"]
            ok = oid in IMPL_OPS
            w(f"<a id=\"op-{oid.lower()}\"></a>\n### `{m.upper()} {path}`\n")
            w(f"**{one_line(op.get('summary', ''))}** (`{oid}`)\n")
            w(f"- Role: **{role_of(op)}**")
            w(f"- Status: **{'implemented' if ok else 'planned (not implemented yet)'}**\n")
            if op.get("description"):
                w(op["description"].strip() + "\n")
            if ok and oid in NOTES:
                w("> **In this build:** " + NOTES[oid] + "\n")
            rows = param_rows(item, op)
            if rows:
                w("**Parameters**\n\n| Name | In | Type | Required | Notes |\n| --- | --- | --- | --- | --- |")
                out.extend(rows)
                w("")
            body = op.get("requestBody")
            if body:
                ct, media = next(iter(body["content"].items()))
                sch = media.get("schema", {})
                w(f"**Request body** (`{ct}`{', required' if body.get('required') else ''}): {type_str(sch)}\n")
                tbl = field_table(sch)
                if tbl:
                    w(tbl)
            w("**Responses**\n\n| Status | Meaning | Body |\n| --- | --- | --- |")
            out.extend(response_rows(op))
            w("")
            if ok:
                w("**Example**\n\n```bash\n" + curl_example(m, path, op, item) + "\n```\n")
    w("## Schemas\n")
    w("Every object the API sends or accepts. Field types link to other schemas.\n")
    for name in sorted(RS, key=str.lower):
        s = RS[name]
        w(f"<a id=\"schema-{name.lower()}\"></a>\n### {name}\n")
        if s.get("description"):
            w(s["description"].strip() + "\n")
        props, _ = object_parts(s)
        if props:
            w(field_table(s))
        else:
            w(f"Type: {type_str(s)}\n")
    return "\n".join(out)


# ── WebSocket reference ────────────────────────────────────────────────────
def gen_ws():
    out = []
    w = out.append
    msgs = ws["components"]["messages"]
    cmd_names = WS["CommandName"]["enum"]
    args_for = {}
    for sname, s in WS.items():
        t = s.get("title", "")
        m = re.match(r"([\w., /]+?) \((.+)\)$", t)
        if sname.endswith("Args") and m:
            for n in re.split(r"\s*/\s*", m.group(1)):
                args_for[n.strip()] = (sname, m.group(2))
    args_for["radio.reset_protection"] = (None, "operator")
    args_for["rtc.close"] = (None, "user")
    topics = WS["Topic"]["enum"]
    ev = {}
    for mname, msg in msgs.items():
        parts = msg.get("payload", {}).get("allOf")
        if not parts:
            continue
        props = parts[-1].get("properties", {})
        if "topic" in props:
            ev[props["topic"]["const"]] = (msg, props.get("data"))
    nimpl = sum(c in IMPL_CMDS for c in cmd_names)
    w("# HERMES station WebSocket API reference (`hermes.v1`)\n")
    w("<!-- GENERATED by api/spec/gen_reference.py from api/spec/api-ws.yaml. Do not edit by hand. -->\n")
    w(f"One WebSocket per client at `/api/v2/ws`, for live state, events and commands. "
      f"**{nimpl} of {len(cmd_names)} commands** and **{sum(t in IMPL_TOPICS for t in topics)} of "
      f"{len(topics)} topics** are implemented in this build; the others are *planned*.\n")
    w("How to connect and use it: [API-USAGE.md](API-USAGE.md#8-real-time-websocket). "
      "The REST API: [API-REFERENCE.md](API-REFERENCE.md). "
      f"The contract: [`api/spec/api-ws.yaml`](../api/spec/api-ws.yaml) (version `{ws['info']['version']}`).\n")
    w("## Connecting\n")
    w("""- `GET /api/v2/ws` with `Sec-WebSocket-Protocol: hermes.v1`.
- Authenticate with the REST session: the `hermes_session` cookie (browsers send it by themselves, and
  must come from the station's own origin) or `Authorization: Bearer <token>` (tools). No session: 401.
- The session's role gates every command. `readonly` may subscribe but runs no commands.
- The first frame is `hello`; a `heartbeat` follows every 5 s. A client silent for 60 s is disconnected.
- A command carries a client-chosen `id` and gets exactly one `reply` with that `id`.
- Subscribing sends a full snapshot (`snapshot: true`), then only what changed, at most 10 events a
  second per topic. `seq` counts events per topic; on a gap, subscribe again for a new snapshot.
""")
    w("## Frames from the client\n")
    w("| Frame | Fields |\n| --- | --- |")
    w("| `subscribe` | `{\"type\":\"subscribe\",\"id\":\"1\",\"topics\":[\"radio.state\"]}` |")
    w("| `unsubscribe` | `{\"type\":\"unsubscribe\",\"id\":\"2\",\"topics\":[\"radio.state\"]}` |")
    w("| `cmd` | `{\"type\":\"cmd\",\"id\":\"42\",\"cmd\":\"radio.set\",\"args\":{...}}` |\n")
    w("## Frames from the station\n")
    w("| Frame | Meaning |\n| --- | --- |")
    w("| `hello` | First frame: your `role`, `user`, `locale`, the `topics` and `commands` available to you, and `limits`. |")
    w("| `heartbeat` | Every 5 s, with the station clock `ts`. |")
    w("| `reply` | The answer to a `subscribe`, `unsubscribe` or `cmd`: `{\"type\":\"reply\",\"id\":...,\"ok\":true,\"data\":...}` or `\"ok\":false` with `error: {code, message}`. |")
    w("| `event` | A topic's snapshot or change: `{\"type\":\"event\",\"topic\":...,\"seq\":...,\"ts\":...,\"snapshot\":...,\"data\":...}`. |")
    w("| binary | Spectrum frames (kinds 3 and 4), to subscribers of `radio.spectrum.*` and `modem.spectrum.rx`. |\n")
    w("## Commands\n")
    w("| Command | Role | Status | Arguments |\n| --- | --- | --- | --- |")
    for c in cmd_names:
        sname, role = args_for.get(c, (None, "?"))
        arg = f"[{sname}](#args-{sname.lower()})" if sname else "none"
        w(f"| `{c}` | {role.split(';')[0].split(' ')[0]} | {'implemented' if c in IMPL_CMDS else 'planned'} | {arg} |")
    w("")
    shown = set()
    for c in cmd_names:
        sname, _ = args_for.get(c, (None, ""))
        if sname and sname not in shown:
            shown.add(sname)
            s = WS[sname]
            w(f"<a id=\"args-{sname.lower()}\"></a>\n### {s.get('title', sname)}\n")
            if s.get("description"):
                w(s["description"].strip() + "\n")
            w(field_table(s, from_ws=True, local=WS))
    w("### Reply error codes\n")
    codes = WS["Reply"]["properties"]["error"]["properties"]["code"]["enum"]
    w(", ".join(f"`{c}`" for c in codes) + "\n")
    w("## Topics\n")
    w("| Topic | Status | Event `data` |\n| --- | --- | --- |")
    for t in topics:
        msg, data = ev.get(t, (None, None))
        d = type_str(data, True) if data else "binary spectrum frame"
        w(f"| `{t}` | {'implemented' if t in IMPL_TOPICS else 'planned'} | {d} |")
    w("")
    for t in topics:
        msg, data = ev.get(t, (None, None))
        if not msg:
            continue
        w(f"### `{t}`\n")
        if msg.get("summary"):
            w(one_line(msg["summary"]) + "\n")
        if data and not data.get("$ref"):
            tbl = field_table(data, True, WS)
            if tbl:
                w(tbl)
    w("### Schemas used by the topics above\n")
    for name in ("ChatLine",):
        sch = WS[name]
        w(f"<a id=\"schema-{name.lower()}\"></a>\n#### {name}\n")
        w(field_table(sch, from_ws=True, local=WS))
    w("### Binary frames\n")
    w("""Little-endian. 12-byte header: `u8 kind`, `u8 version` (1), `u16 flags` (0), `u32 seq`, `u32 sample_rate`.

| kind | Topic | Payload |
| --- | --- | --- |
| 3 | `radio.spectrum.rx`, `modem.spectrum.rx` | `u16 nbins`, `f32 start_hz`, `f32 bin_hz`, `f32 dB x nbins` |
| 4 | `radio.spectrum.tx` | as kind 3 |
""")
    return "\n".join(out)


def main():
    (DOCS / "API-REFERENCE.md").write_text(gen_rest().rstrip() + "\n")
    (DOCS / "API-WS-REFERENCE.md").write_text(gen_ws().rstrip() + "\n")
    print(f"wrote {DOCS/'API-REFERENCE.md'} and {DOCS/'API-WS-REFERENCE.md'}")
    print(f"implemented: {len(IMPL_OPS)} operations, {len(IMPL_CMDS)} commands, {len(IMPL_TOPICS)} topics")


if __name__ == "__main__":
    sys.exit(main())
