package spec

import "testing"

// The reader finds the contract's operations with their method, path, role
// and public flag, and the hermes.v1 commands.
func TestOperations(t *testing.T) {
	ops := Operations()
	if len(ops) < 90 {
		t.Fatalf("read %d operations from api-rest.yaml; the contract has about 100", len(ops))
	}
	cases := []Operation{
		{ID: "getHealth", Method: "GET", Path: "/health", Public: true},
		{ID: "getRadioState", Method: "GET", Path: "/radio", Role: RoleReadonly},
		{ID: "updateProfile", Method: "PATCH", Path: "/radio/profiles/{profile}", Role: RoleOperator},
		{ID: "selectProfile", Method: "PUT", Path: "/radio/active-profile", Role: RoleOperator},
		{ID: "resetProtection", Method: "POST", Path: "/radio/protection/reset", Role: RoleOperator},
		{ID: "getDigiLog", Method: "GET", Path: "/digi/log", Role: RoleReadonly},
		{ID: "reboot", Method: "POST", Path: "/system/reboot", Role: RoleAdmin},
	}
	for _, want := range cases {
		got, err := Op(want.ID)
		if err != nil {
			t.Errorf("%v", err)
			continue
		}
		if got != want {
			t.Errorf("%s = %+v, want %+v", want.ID, got, want)
		}
	}
	if _, err := Op("noSuchOperation"); err == nil {
		t.Error("an unknown operationId was found")
	}
	for id, op := range ops {
		if op.Method == "" || op.Path == "" || (op.Role == RoleNone && !op.Public) {
			t.Errorf("%s: incomplete %+v (every non-public operation has x-hermes-role)", id, op)
		}
	}
}

func TestCommands(t *testing.T) {
	for _, c := range []string{"radio.set", "radio.select_profile", "radio.reset_protection", "rtc.offer"} {
		if !HasCommand(c) {
			t.Errorf("command %s not found in api-ws.yaml (read %v)", c, Commands())
		}
	}
	if HasCommand("radio.nonsense") {
		t.Error("a command that is not in the contract was found")
	}
}

func TestTopics(t *testing.T) {
	got := Topics()
	if len(got) != 15 {
		t.Fatalf("read %d topics from api-ws.yaml, want 15: %v", len(got), got)
	}
	for _, name := range []string{"radio.state", "radio.spectrum.rx", "digi", "modem.status", "reticulum"} {
		if !HasTopic(name) {
			t.Errorf("topic %s not found (read %v)", name, got)
		}
	}
	if HasTopic("radio.nonsense") || HasTopic("radio.set") {
		t.Error("a name that is not a topic was found")
	}
}

// Statuses reads each operation's documented responses, including $ref ones.
func TestStatuses(t *testing.T) {
	st := Statuses()
	has := func(id string, code int) bool {
		for _, c := range st[id] {
			if c == code {
				return true
			}
		}
		return false
	}
	for id, code := range map[string]int{
		"login": 201, "logout": 204, "getSession": 200, // inline
		"runSetup": 409, "getProfile": 404, "updateProfile": 422, // $ref
	} {
		if !has(id, code) {
			t.Errorf("%s: %d not among documented statuses %v", id, code, st[id])
		}
	}
	if has("getHealth", 201) {
		t.Errorf("getHealth documents 201: %v", st["getHealth"])
	}
	for id := range Operations() {
		if len(st[id]) == 0 {
			t.Errorf("%s documents no responses", id)
		}
	}
}
