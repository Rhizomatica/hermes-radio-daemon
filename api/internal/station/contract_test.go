package station_test

import (
	"encoding/json"
	"net/http"
	"regexp"
	"strings"
	"testing"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
	"github.com/Rhizomatica/hermes-radio-daemon/api/spec"
)

// roleOrder is the roles from lowest to highest.
var roleOrder = []spec.Role{spec.RoleReadonly, spec.RoleUser, spec.RoleOperator, spec.RoleAdmin}

// Statuses any operation may answer without documenting them one by one: the
// request itself was refused before or by validation. 400, 404, 415, 422 and
// 429 are the request-level refusals; 413 is the body-size ceiling; 401 and
// 403 are the gate's, and are asserted strictly on their own (no session ->
// 401, a lower role -> 403), so here they only keep a public operation's own
// 401 (login: wrong credentials) from being called undocumented.
var generic = map[int]bool{400: true, 401: true, 403: true, 404: true, 413: true, 415: true, 422: true, 429: true}

// gateDetail is what the router's own login gate says. A public operation may
// answer 401 itself (login does, for wrong credentials) but never with this.
const gateDetail = "not logged in, or the session ended"

var pathParam = regexp.MustCompile(`\{(\w+)\}`)

// sample is the value for a {param}, by name. profile 0 exists on the stub
// radio; the other implemented parameters are numeric ids, and 999999 is one
// nothing has, so the operation answers 404 from its handler. A new
// operation whose parameter is not an id (a filename, a callsign) must be
// given a value here: fillPath fails the test on a name it does not know,
// rather than send a wrong kind of value.
var sample = map[string]string{"profile": "0", "user_id": "999999", "session_id": "999999"}

func fillPath(t *testing.T, p string) string {
	return pathParam.ReplaceAllStringFunc(p, func(m string) string {
		v, ok := sample[m[1:len(m)-1]]
		if !ok {
			t.Fatalf("path parameter %s has no sample value: add it to sample in contract_test.go", m)
		}
		return v
	})
}

// volatile are fields that change on their own during the run: every reply's
// request_id, and last_login_at (the logout subtest logs in again).
var volatile = regexp.MustCompile(`"(request_id|last_login_at)":("[^"]*"|null)`)

// snapshot is every parameterless GET an admin can make, by path: the state
// the station shows. Sessions are left out (the test logs in and out).
func snapshot(e *testapi.Env, token string) map[string]string {
	out := map[string]string{}
	for _, id := range e.R.Operations() {
		op := spec.Operations()[id]
		if op.Method != "GET" || strings.Contains(op.Path, "{") || strings.HasPrefix(op.Path, "/session") {
			continue
		}
		r := e.DoAuth("GET", op.Path, "", token)
		out[op.Path] = volatile.ReplaceAllString(string(r.Body), "")
	}
	return out
}

// TestContract walks every operation the router registers, whatever it is,
// so a new endpoint is covered the moment it is registered. For each it
// checks what the spec promises:
//
//   - no session: 401 unauthenticated (public operations are open);
//   - every role below x-hermes-role: 403 forbidden;
//   - the role itself reaches the handler (not 401/403/405, not an unrouted
//     404, no 5xx), and the answer is a status the spec documents;
//   - every reply has X-Request-Id; errors are problem+json with a matching
//     status, a code and the same request_id; 204 has no body.
func TestContract(t *testing.T) {
	e := testapi.NewAuth(t)
	login := func(r spec.Role) string {
		return e.Login(string(r)+"1", "password-"+string(r))
	}
	tokens := map[spec.Role]string{}
	for _, r := range roleOrder {
		e.SeedUser(string(r)+"1", "password-"+string(r), string(r))
		tokens[r] = login(r)
	}

	ids := e.R.Operations()
	if len(ids) < 30 {
		t.Fatalf("the router registers %d operations; expected at least the 30 implemented today", len(ids))
	}
	statuses := spec.Statuses()

	// The calls below use an id nothing has and the empty object, which a
	// handler must refuse before it changes anything. The subtests share one
	// station, so check that after all of them it shows what it showed before.
	before := snapshot(e, tokens[spec.RoleAdmin])
	defer func() {
		after := snapshot(e, tokens[spec.RoleAdmin])
		for path, b := range before {
			if after[path] != b {
				t.Errorf("GET %s changed during the contract run (a refused call mutated state):\nbefore: %s\nafter:  %s", path, b, after[path])
			}
		}
	}()

	for _, id := range ids {
		op := spec.Operations()[id]
		path := fillPath(t, op.Path)
		body := ""
		if op.Method == "POST" || op.Method == "PUT" || op.Method == "PATCH" {
			body = "{}"
		}
		t.Run(id, func(t *testing.T) {
			// The token to use for the "reaches the handler" call. logout
			// would end the shared token, so it gets a fresh session.
			asRole := op.Role
			if op.Public {
				asRole = spec.RoleReadonly
			}
			token := tokens[asRole]
			if id == "logout" {
				token = login(asRole)
			}

			if op.Public {
				r := e.Do(op.Method, path, body)
				if r.Status == 403 || (r.Status == 401 && problemDetail(r) == gateDetail) {
					t.Errorf("public %s %s without a session was stopped by the gate (%d): %s", op.Method, op.Path, r.Status, r.Body)
				}
			} else {
				r := e.Do(op.Method, path, body)
				checkProblem(t, r, 401, "unauthenticated")
			}

			for _, lower := range roleOrder {
				if op.Public || !below(lower, op.Role) {
					continue
				}
				r := e.DoAuth(op.Method, path, body, tokens[lower])
				if r.Status != 403 {
					t.Errorf("role %s on %s %s = %d, want 403 (needs %s)", lower, op.Method, op.Path, r.Status, op.Role)
					continue
				}
				checkProblem(t, r, 403, "forbidden")
			}

			r := e.DoAuth(op.Method, path, body, token)
			switch {
			case r.Status == 401 && !op.Public, r.Status == 405, r.Status >= 500 && !absent(r):
				t.Fatalf("%s %s as %s = %d: %s", op.Method, op.Path, asRole, r.Status, r.Body)
			case r.Status == 403 && problemCode(r) != "":
				t.Fatalf("%s %s as %s refused its own role: %s", op.Method, op.Path, asRole, r.Body)
			case r.Status == 404 && strings.HasPrefix(problemDetail(r), "no such resource"):
				t.Fatalf("%s %s is registered but the router does not route it: %s", op.Method, op.Path, r.Body)
			}
			documented := false
			for _, c := range statuses[id] {
				documented = documented || c == r.Status
			}
			if !documented && !generic[r.Status] {
				t.Errorf("%s %s answered %d, which the spec does not document (documents %v): %s",
					op.Method, op.Path, r.Status, statuses[id], r.Body)
			}
			checkReply(t, r)
		})
	}
}

// absent is the documented 503 of an operation whose backend is not there
// (the stub radio runs no mercury): a refusal, not a failure.
func absent(r testapi.Response) bool {
	c := problemCode(r)
	return r.Status == 503 && (c == "not_running" || c == "not_supported")
}

func below(a, b spec.Role) bool {
	ra, rb := -1, -1
	for i, r := range roleOrder {
		if r == a {
			ra = i
		}
		if r == b {
			rb = i
		}
	}
	return ra < rb
}

func problemCode(r testapi.Response) string {
	var p struct {
		Code string `json:"code"`
	}
	if r.Header.Get("Content-Type") == "application/problem+json" {
		_ = json.Unmarshal(r.Body, &p)
	}
	return p.Code
}

func problemDetail(r testapi.Response) string {
	var p struct {
		Detail string `json:"detail"`
	}
	_ = json.Unmarshal(r.Body, &p)
	return p.Detail
}

// checkProblem wants an error reply of this status and code.
func checkProblem(t *testing.T, r testapi.Response, status int, code string) {
	t.Helper()
	if r.Status != status {
		t.Errorf("status = %d, want %d: %s", r.Status, status, r.Body)
		return
	}
	if got := problemCode(r); got != code {
		t.Errorf("code = %q, want %q: %s", got, code, r.Body)
	}
	checkReply(t, r)
}

// checkReply checks what every reply promises, whatever the operation.
func checkReply(t *testing.T, r testapi.Response) {
	t.Helper()
	rid := r.Header.Get("X-Request-Id")
	if rid == "" {
		t.Errorf("no X-Request-Id on a %d reply", r.Status)
	}
	switch {
	case r.Status == http.StatusNoContent:
		if len(r.Body) != 0 {
			t.Errorf("204 with a body: %s", r.Body)
		}
	case r.Status >= 400:
		if ct := r.Header.Get("Content-Type"); ct != "application/problem+json" {
			t.Errorf("%d reply is %q, want application/problem+json", r.Status, ct)
			return
		}
		var p struct {
			Type      string `json:"type"`
			Title     string `json:"title"`
			Status    int    `json:"status"`
			Code      string `json:"code"`
			RequestID string `json:"request_id"`
		}
		if err := json.Unmarshal(r.Body, &p); err != nil {
			t.Errorf("problem body is not JSON: %v: %s", err, r.Body)
			return
		}
		if p.Status != r.Status || p.Code == "" || p.Title == "" || p.Type == "" {
			t.Errorf("incomplete problem for a %d: %s", r.Status, r.Body)
		}
		if p.RequestID != rid {
			t.Errorf("problem request_id %q != X-Request-Id %q", p.RequestID, rid)
		}
	default:
		if ct := r.Header.Get("Content-Type"); !strings.HasPrefix(ct, "application/json") {
			t.Errorf("%d reply is %q, want application/json", r.Status, ct)
		}
		if !json.Valid(r.Body) {
			t.Errorf("%d body is not JSON: %s", r.Status, r.Body)
		}
	}
}
