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
// request itself was refused before or by validation.
var generic = map[int]bool{400: true, 401: true, 403: true, 404: true, 413: true, 415: true, 422: true, 429: true}

// undocumented503 are answers the code gives that the pinned api-rest.yaml
// does not list: with no mercury running, the modem operations say 503
// not_running (docs/API-WRITERS.md). That is a gap in the contract, to be
// proposed to hermes-documentation; until the pinned spec has it, it is
// named here instead of hidden. TestContractAllowlistIsCurrent fails once
// the spec documents one, so an entry cannot outlive the gap.
var undocumented503 = map[string]bool{
	"getModemStatus":    true,
	"getModemConfig":    true,
	"updateModemConfig": true,
}

// gateDetail is what the router's own login gate says. A public operation may
// answer 401 itself (login does, for wrong credentials) but never with this.
const gateDetail = "not logged in, or the session ended"

var pathParam = regexp.MustCompile(`\{(\w+)\}`)

// fillPath puts a value in each {param}: profile 0 exists on the stub radio;
// everything else is an id nothing has, so an operation that takes one
// answers 404 from its handler and never changes anything.
func fillPath(p string) string {
	return pathParam.ReplaceAllStringFunc(p, func(m string) string {
		if m == "{profile}" {
			return "0"
		}
		return "999999"
	})
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

	for _, id := range ids {
		op := spec.Operations()[id]
		path := fillPath(op.Path)
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
			if !documented && r.Status == 503 && undocumented503[id] && absent(r) {
				documented = true
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

// An allowlist entry is dropped as soon as the spec documents the status.
func TestContractAllowlistIsCurrent(t *testing.T) {
	for id := range undocumented503 {
		for _, c := range spec.Statuses()[id] {
			if c == 503 {
				t.Errorf("%s: the spec now documents 503; remove it from undocumented503", id)
			}
		}
	}
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
