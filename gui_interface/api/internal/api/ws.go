package api

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"sort"
	"time"

	"github.com/Rhizomatica/hermes-radio-daemon/gui_interface/api/spec"
)

// hermes.v1 (spec/api-ws.yaml), stage 1: hello, heartbeat, and commands
// with exactly one reply each. Topics (subscribe, events) come in stage 2.
//
// radiod's C websocket server owns the socket and hands each text frame to
// WSMessage; frames go back through radio.WSSend/WSBroadcast. Commands run
// in order on one goroutine per connection, off the socket thread, so a
// slow one (a CAT read) never stalls other clients.

// Error codes of a reply (api-ws.yaml Reply.error.code).
const (
	CodeBadRequest   = "bad_request"
	CodeUnknownCmd   = "unknown_cmd"
	CodeMissingArg   = "missing_arg"
	CodeOutOfRange   = "out_of_range"
	CodeForbidden    = "forbidden"
	CodeBusy         = "busy"
	CodeNotSupported = "not_supported"
	CodeQueueFull    = "queue_full"
	CodeInternal     = "internal"
)

// CmdError is a command's failure: a code from the list above and a message.
type CmdError struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

func (e *CmdError) Error() string { return e.Code + ": " + e.Message }

// Errf makes a CmdError.
func Errf(code, format string, a ...any) *CmdError {
	return &CmdError{Code: code, Message: fmt.Sprintf(format, a...)}
}

// CommandFunc runs one command: its result is the reply's data.
type CommandFunc func(ctx context.Context, c *Conn, args json.RawMessage) (any, *CmdError)

type command struct {
	role spec.Role
	h    CommandFunc
}

// Command registers a hermes.v1 command. It panics if api-ws.yaml has no
// such command, or on a second registration.
func (rt *Router) Command(name string, role spec.Role, h CommandFunc) {
	if !spec.HasCommand(name) {
		panic(fmt.Sprintf("api: no command %q in api-ws.yaml", name))
	}
	if _, dup := rt.commands[name]; dup {
		panic(fmt.Sprintf("api: command %s registered twice", name))
	}
	rt.commands[name] = command{role, h}
}

// Commands lists the registered commands a role may run, sorted.
func (rt *Router) Commands(role spec.Role) []string {
	out := []string{}
	for name, c := range rt.commands {
		if AtLeast(role, c.role) {
			out = append(out, name)
		}
	}
	sort.Strings(out)
	return out
}

// Conn is one hermes.v1 connection.
type Conn struct {
	ID   int64
	Role spec.Role
	rt   *Router
	in   chan []byte
	done chan struct{}
}

const (
	maxFrame        = 65536
	connQueue       = 32
	heartbeatPeriod = 5 * time.Second
)

// WSOpen checks an upgrade to /api/v2/ws and, on success, starts the
// connection and sends its hello. It returns 0 to accept, or the HTTP
// status to refuse with (401 when sessions exist and there is none).
func (rt *Router) WSOpen(id int64, r *http.Request) int {
	role := rt.RoleOf(r)
	if roleRank[role] == 0 {
		return http.StatusUnauthorized
	}
	c := &Conn{ID: id, Role: role, rt: rt, in: make(chan []byte, connQueue), done: make(chan struct{})}
	rt.conns.Store(id, c)
	rt.hbStart.Do(func() { go rt.heartbeat() })
	c.send(hello{
		Type: "hello", Protocol: "hermes.v1", Service: "hermes-radio-daemon",
		Version: rt.radio.Version(), Role: string(role),
		Topics: []string{}, Commands: rt.Commands(role),
		Limits: map[string]int{"max_frame_bytes": maxFrame},
	})
	go c.run()
	return 0
}

// WSMessage takes one text frame from the socket thread; it never blocks.
func (rt *Router) WSMessage(id int64, frame []byte) {
	v, ok := rt.conns.Load(id)
	if !ok {
		return
	}
	c := v.(*Conn)
	select {
	case c.in <- frame:
	default:
		var head struct {
			ID string `json:"id"`
		}
		_ = json.Unmarshal(frame, &head)
		c.reply(head.ID, nil, Errf(CodeQueueFull, "too many commands in flight"))
	}
}

// WSClose ends a connection.
func (rt *Router) WSClose(id int64) {
	if v, ok := rt.conns.LoadAndDelete(id); ok {
		close(v.(*Conn).done)
	}
}

func (rt *Router) heartbeat() {
	t := time.NewTicker(heartbeatPeriod)
	defer t.Stop()
	for range t.C {
		open := false
		rt.conns.Range(func(_, _ any) bool { open = true; return false })
		if !open {
			continue
		}
		b, _ := json.Marshal(heartbeat{"heartbeat", time.Now().UTC().Format("2006-01-02T15:04:05.000Z")})
		_ = rt.radio.WSBroadcast(b)
	}
}

func (c *Conn) send(v any) {
	b, err := json.Marshal(v)
	if err == nil {
		_ = c.rt.radio.WSSend(c.ID, b)
	}
}

// The frames, as structs so their fields keep api-ws.yaml's order (type first).
type hello struct {
	Type     string         `json:"type"`
	Protocol string         `json:"protocol"`
	Service  string         `json:"service"`
	Version  string         `json:"version"`
	Role     string         `json:"role"`
	Topics   []string       `json:"topics"`
	Commands []string       `json:"commands"`
	Limits   map[string]int `json:"limits"`
}

type heartbeat struct {
	Type string `json:"type"`
	TS   string `json:"ts"`
}

type reply struct {
	Type  string    `json:"type"`
	ID    string    `json:"id"`
	OK    bool      `json:"ok"`
	Data  any       `json:"data,omitempty"`
	Error *CmdError `json:"error,omitempty"`
}

func (c *Conn) reply(id string, data any, cerr *CmdError) {
	r := reply{Type: "reply", ID: id, OK: cerr == nil, Error: cerr}
	if cerr == nil {
		r.Data = data
	}
	c.send(r)
}

func (c *Conn) run() {
	for {
		select {
		case <-c.done:
			return
		case frame := <-c.in:
			c.handle(frame)
		}
	}
}

func (c *Conn) handle(frame []byte) {
	var f struct {
		Type string          `json:"type"`
		ID   string          `json:"id"`
		Cmd  string          `json:"cmd"`
		Args json.RawMessage `json:"args"`
	}
	if len(frame) > maxFrame {
		c.reply("", nil, Errf(CodeBadRequest, "frame larger than %d bytes", maxFrame))
		return
	}
	if err := json.Unmarshal(frame, &f); err != nil {
		c.reply("", nil, Errf(CodeBadRequest, "not a JSON object"))
		return
	}
	if len(f.ID) > 64 {
		c.reply("", nil, Errf(CodeBadRequest, "id longer than 64 characters"))
		return
	}
	switch f.Type {
	case "cmd":
		cmd, ok := c.rt.commands[f.Cmd]
		if !ok {
			c.reply(f.ID, nil, Errf(CodeUnknownCmd, "no command %q", f.Cmd))
			return
		}
		if !AtLeast(c.Role, cmd.role) {
			c.reply(f.ID, nil, Errf(CodeForbidden, "%s needs the %s role", f.Cmd, cmd.role))
			return
		}
		if len(f.Args) == 0 || string(f.Args) == "null" {
			f.Args = json.RawMessage("{}")
		}
		data, cerr := c.safeRun(cmd.h, f.Args)
		c.reply(f.ID, data, cerr)
	case "subscribe", "unsubscribe":
		c.reply(f.ID, nil, Errf(CodeNotSupported, "topics are not served yet"))
	default:
		c.reply(f.ID, nil, Errf(CodeBadRequest, "unknown frame type %q", f.Type))
	}
}

// safeRun turns a handler's panic into an internal error, so one bad
// handler cannot take the daemon (and the radio) down with it.
func (c *Conn) safeRun(h CommandFunc, args json.RawMessage) (data any, cerr *CmdError) {
	defer func() {
		if p := recover(); p != nil {
			data, cerr = nil, Errf(CodeInternal, "command failed")
		}
	}()
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	return h(ctx, c, args)
}

// DecodeArgs reads a command's args strictly (no unknown fields); on
// failure the CmdError says why.
func DecodeArgs(args json.RawMessage, dst any) *CmdError {
	dec := json.NewDecoder(bytesReader(args))
	dec.DisallowUnknownFields()
	if err := dec.Decode(dst); err != nil {
		return Errf(CodeBadRequest, "args: %s", jsonErr(err))
	}
	return nil
}
