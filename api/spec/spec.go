// Package spec is the station API's contract (README.md): the pinned
// api-rest.yaml and api-ws.yaml, embedded, and what the router needs from
// them, read without a YAML dependency.
//
// The reader understands exactly the layout these two files use (two-space
// indentation, one key per line); spec_test.go checks it finds what it
// should, so a layout change in the contract fails the tests, not silently.
package spec

import (
	_ "embed"
	"fmt"
	"regexp"
	"sort"
	"strings"
)

//go:embed api-rest.yaml
var restYAML string

//go:embed api-ws.yaml
var wsYAML string

// Role is the lowest role allowed to call an operation (x-hermes-role).
type Role string

const (
	RoleNone     Role = ""         // security: [] (no login, e.g. getHealth)
	RoleReadonly Role = "readonly" // everything below sees what user sees
	RoleUser     Role = "user"
	RoleOperator Role = "operator"
	RoleAdmin    Role = "admin"
)

// Operation is one REST operation of api-rest.yaml.
type Operation struct {
	ID     string // operationId
	Method string // GET, POST, PUT, PATCH, DELETE
	Path   string // e.g. /radio/profiles/{profile}, under BasePath
	Role   Role
	Public bool // security: [] (no login needed)
}

// BasePath is where every path of api-rest.yaml lives.
const BasePath = "/api/v2"

var (
	reOpPath    = regexp.MustCompile(`^  (/\S*):\s*$`)
	reOpVerb    = regexp.MustCompile(`^    (get|post|put|patch|delete):`)
	reOpID      = regexp.MustCompile(`^      operationId:\s*(\S+)`)
	reOpRole    = regexp.MustCompile(`^      x-hermes-role:\s*(\S+)`)
	reOpOpen    = regexp.MustCompile(`^      security:\s*\[\]`)
	reTopKey    = regexp.MustCompile(`^[A-Za-z]`)
	reCmdName   = regexp.MustCompile(`^        - ([a-z]+\.[a-z_.]+)\s*$`)
	reTopicName = regexp.MustCompile(`^        - ([a-z]+(\.[a-z_.]+)?)\s*$`)
)

var operations map[string]Operation

// Operations returns every operation of api-rest.yaml, by operationId.
func Operations() map[string]Operation {
	if operations != nil {
		return operations
	}
	ops := map[string]Operation{}
	var path, verb string
	var cur *Operation
	inPaths := false
	flush := func() {
		if cur != nil && cur.ID != "" {
			ops[cur.ID] = *cur
		}
		cur = nil
	}
	for _, l := range strings.Split(restYAML, "\n") {
		if strings.HasPrefix(l, "paths:") {
			inPaths = true
			continue
		}
		if inPaths && reTopKey.MatchString(l) {
			flush()
			inPaths = false
		}
		if !inPaths {
			continue
		}
		if m := reOpPath.FindStringSubmatch(l); m != nil {
			flush()
			path = m[1]
			continue
		}
		if m := reOpVerb.FindStringSubmatch(l); m != nil {
			flush()
			verb = strings.ToUpper(m[1])
			cur = &Operation{Method: verb, Path: path}
			continue
		}
		if cur == nil {
			continue
		}
		if m := reOpID.FindStringSubmatch(l); m != nil {
			cur.ID = m[1]
		} else if m := reOpRole.FindStringSubmatch(l); m != nil {
			cur.Role = Role(m[1])
		} else if reOpOpen.MatchString(l) {
			cur.Public = true
		}
	}
	flush()
	operations = ops
	return ops
}

var (
	reRespOpen = regexp.MustCompile(`^      responses:\s*$`)
	reRespCode = regexp.MustCompile(`^        ["']?([0-9]{3})["']?:`)
	reIndent6  = regexp.MustCompile(`^ {0,6}\S`)
)

var statuses map[string][]int

// Statuses returns, by operationId, the HTTP statuses each operation of
// api-rest.yaml documents under responses (including the ones it takes from
// components/responses by $ref), in file order.
func Statuses() map[string][]int {
	if statuses != nil {
		return statuses
	}
	out := map[string][]int{}
	inPaths, inResp := false, false
	id := ""
	for _, l := range strings.Split(restYAML, "\n") {
		if strings.HasPrefix(l, "paths:") {
			inPaths = true
			continue
		}
		if inPaths && reTopKey.MatchString(l) {
			break
		}
		if !inPaths {
			continue
		}
		if m := reOpID.FindStringSubmatch(l); m != nil {
			id, inResp = m[1], false
			continue
		}
		if reOpPath.MatchString(l) || reOpVerb.MatchString(l) {
			id, inResp = "", false
			continue
		}
		if reRespOpen.MatchString(l) {
			inResp = true
			continue
		}
		if inResp {
			if m := reRespCode.FindStringSubmatch(l); m != nil && id != "" {
				var code int
				fmt.Sscanf(m[1], "%d", &code)
				out[id] = append(out[id], code)
			} else if reIndent6.MatchString(l) {
				inResp = false // the next key of the operation
			}
		}
	}
	statuses = out
	return out
}

// Op returns one operation, or an error naming the unknown operationId.
func Op(id string) (Operation, error) {
	op, ok := Operations()[id]
	if !ok {
		return Operation{}, fmt.Errorf("spec: no operation %q in api-rest.yaml", id)
	}
	return op, nil
}

var commands []string

// Commands returns the hermes.v1 command names of api-ws.yaml (CommandName).
func Commands() []string {
	if commands != nil {
		return commands
	}
	in := false
	var out []string
	for _, l := range strings.Split(wsYAML, "\n") {
		if strings.TrimSpace(l) == "CommandName:" {
			in = true
			continue
		}
		if !in {
			continue
		}
		if m := reCmdName.FindStringSubmatch(l); m != nil {
			out = append(out, m[1])
		} else if strings.TrimSpace(l) != "" && !strings.Contains(l, "type: string") &&
			!strings.Contains(l, "enum:") {
			break
		}
	}
	sort.Strings(out)
	commands = out
	return out
}

var topics []string

// Topics returns the hermes.v1 topic names of api-ws.yaml (Topic).
func Topics() []string {
	if topics != nil {
		return topics
	}
	in, inEnum := false, false
	var out []string
	for _, l := range strings.Split(wsYAML, "\n") {
		t := strings.TrimSpace(l)
		if l == "    Topic:" {
			in = true
			continue
		}
		if !in {
			continue
		}
		if t == "enum:" {
			inEnum = true
			continue
		}
		if m := reTopicName.FindStringSubmatch(l); inEnum && m != nil {
			out = append(out, m[1])
		} else if t != "" && t != "type: string" {
			break
		}
	}
	sort.Strings(out)
	topics = out
	return out
}

// HasTopic reports whether api-ws.yaml defines the topic.
func HasTopic(name string) bool {
	for _, t := range Topics() {
		if t == name {
			return true
		}
	}
	return false
}

// HasCommand reports whether api-ws.yaml defines the command.
func HasCommand(name string) bool {
	for _, c := range Commands() {
		if c == name {
			return true
		}
	}
	return false
}
