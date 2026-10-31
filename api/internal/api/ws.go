package api

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"sort"
	"sync"
	"time"

	"github.com/Rhizomatica/hermes-radio-daemon/api/spec"
)

// hermes.v1 (spec/api-ws.yaml): hello, heartbeat, commands with exactly
// one reply each, and topics (topics.go).
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
	CodeNoLink       = "no_link"
	CodeNotSupported = "not_supported"
	CodeRigTimeout   = "rig_timeout"
	CodeQueueFull    = "queue_full"
	CodeInternal     = "internal"
	CodeNoPTT        = "no_ptt"
	CodeRateLimited  = "rate_limited"
	CodeWrongMode    = "wrong_mode"
	CodeTxTimeout    = "tx_timeout"
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

// OnClose registers what a domain releases for a closed hermes.v1
// connection (radio.ptt: the transmitter it keyed). It runs once the
// connection's last command has, so it also catches a command that was
// still running when radiod's C server released the connection itself.
// Register before connections open.
func (rt *Router) OnClose(f func(c *Conn)) { rt.closers = append(rt.closers, f) }

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

	// cred is the upgrade request's credentials (headers and peer, no
	// body), and session its session: the connection lasts only as long as
	// they still resolve to that session and role (sessionLive).
	cred    *http.Request
	session int64
	// User is the session's user (0 without sessions): per-user limits, and
	// who transmitted, for the log.
	User int64

	// mu orders this connection's events: its goroutine (snapshots) and
	// the publisher both send under it.
	mu   sync.Mutex
	subs map[string]*subscription
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
	if rt.Resolve == nil && !loopbackPeer(r.RemoteAddr) {
		rt.wsLog.Info("refused: remote with no sessions", "conn", id, "peer", r.RemoteAddr)
		return http.StatusForbidden
	}
	role, session, user := rt.RoleOf(r), int64(0), int64(0)
	if rt.Resolve != nil {
		p, ok := rt.Resolve(r)
		role, session, user = p.Role, p.SessionID, p.UserID
		if !ok {
			role = spec.RoleNone
		}
	}
	if roleRank[role] == 0 {
		rt.wsLog.Info("refused: no session", "conn", id, "peer", r.RemoteAddr)
		return http.StatusUnauthorized
	}
	// api-ws.yaml: only same-origin browsers are accepted. A request with
	// the session cookie must come with an Origin naming this station (the
	// same rule as the REST CSRF check); a Bearer-only request, which a
	// browser never makes on an upgrade, is exempt. Only enforced once
	// sessions exist (Resolve set); the loopback-admin router has no
	// sessions and keeps answering without an Origin.
	if rt.Resolve != nil && HasSessionCookie(r) && !SameOrigin(r) {
		rt.wsLog.Info("refused: bad origin", "conn", id, "peer", r.RemoteAddr)
		return http.StatusForbidden
	}
	c := &Conn{ID: id, Role: role, rt: rt, in: make(chan []byte, connQueue), done: make(chan struct{}),
		cred: r.Clone(context.Background()), session: session, User: user, subs: map[string]*subscription{}}
	c.send(hello{
		Type: "hello", Protocol: "hermes.v1", Service: "hermes-radio-daemon",
		Version: rt.radio.Version(), Role: string(role),
		Topics: rt.Topics(role), Commands: rt.Commands(role),
		Limits: map[string]int{"max_frame_bytes": maxFrame, "max_events_per_s": int(time.Second / publishPeriod)},
	})
	rt.conns.Store(id, c)
	rt.hbStart.Do(func() { go rt.heartbeat(); go rt.publish() })
	rt.wsLog.Info("open", "conn", id, "role", string(role), "peer", r.RemoteAddr)
	go c.run()
	return 0
}

// Done is closed when the connection closes: a domain's goroutine for this
// connection (a rig.watch poller) stops on it.
func (c *Conn) Done() <-chan struct{} { return c.done }

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
		rt.wsLog.Info("closed", "conn", id)
	}
}

// closeSessionEnded is the WebSocket close code for a connection whose
// session ended or whose role changed (api-ws.yaml): 1008, policy violation.
const closeSessionEnded = 1008

// sessionLive reports whether the connection's credentials still resolve to
// the session and role it opened with. Logout, revocation, expiry, a
// disabled or deleted user, or a new role all end it. Without sessions
// (the loopback-admin router) there is nothing to end.
func (c *Conn) sessionLive() (bool, string) {
	if c.rt.Resolve == nil || c.cred == nil {
		return true, ""
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	p, ok := c.rt.Resolve(c.cred.WithContext(ctx))
	switch {
	case !ok || p.SessionID != c.session:
		return false, "session ended"
	case p.Role != c.Role:
		return false, "role changed; connect again"
	}
	return true, ""
}

// end closes the connection from the station's side: it stops here at once
// (no further command, event or heartbeat), and radiod's C server sends
// the close frame and then calls WSClose, which finds it already gone.
func (c *Conn) end(reason string) {
	if _, ok := c.rt.conns.LoadAndDelete(c.ID); !ok {
		return
	}
	close(c.done)
	_ = c.rt.radio.WSClose(c.ID, closeSessionEnded, reason)
	c.rt.wsLog.Info("closed by the station", "conn", c.ID, "reason", reason)
}

func (rt *Router) heartbeat() {
	t := time.NewTicker(heartbeatPeriod)
	defer t.Stop()
	for range t.C {
		open := false
		rt.conns.Range(func(_, v any) bool {
			// A connection whose session ended while it sat idle (only
			// subscribed, or quiet) is closed within one heartbeat.
			c := v.(*Conn)
			if live, why := c.sessionLive(); !live {
				c.end(why)
			} else {
				open = true
			}
			return true
		})
		if !open {
			continue
		}
		b, _ := json.Marshal(heartbeat{"heartbeat", stamp(time.Now())})
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
	defer c.release()
	for {
		select {
		case <-c.done:
			return
		case frame := <-c.in:
			select {
			case <-c.done: // closed while it waited: it does not run
				return
			default:
			}
			c.handle(frame)
		}
	}
}

// release runs when the connection's goroutine ends, after its last
// command: C's spectrum subscriptions and what the domains registered with
// OnClose. C drops them on close too, but a command it raced with may have
// taken them again since.
func (c *Conn) release() {
	c.mu.Lock()
	var binaries []BinaryFunc
	for name := range c.subs {
		if t := c.rt.topics[name]; t.binary != nil {
			binaries = append(binaries, t.binary)
		}
	}
	c.mu.Unlock()
	for _, b := range binaries {
		_ = b(c.ID, false)
	}
	for _, f := range c.rt.closers {
		f(c)
	}
}

func (c *Conn) handle(frame []byte) {
	var f struct {
		Type   string          `json:"type"`
		ID     string          `json:"id"`
		Cmd    string          `json:"cmd"`
		Args   json.RawMessage `json:"args"`
		Topics []string        `json:"topics"`
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
	// Every command and subscription runs on a session that is still live:
	// one ended since the upgrade closes the connection instead.
	if live, why := c.sessionLive(); !live {
		c.end(why)
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
		start := time.Now()
		data, cerr := c.safeRun(f.Cmd, cmd.h, f.Args)
		c.reply(f.ID, data, cerr)
		if cerr != nil {
			c.rt.wsLog.Debug("cmd "+f.Cmd, "conn", c.ID, "id", f.ID, "ok", false, "code", cerr.Code,
				"ms", float64(time.Since(start).Microseconds())/1000)
		} else {
			c.rt.wsLog.Debug("cmd "+f.Cmd, "conn", c.ID, "id", f.ID, "ok", true,
				"ms", float64(time.Since(start).Microseconds())/1000)
		}
	case "subscribe":
		c.subscribe(f.ID, f.Topics)
	case "unsubscribe":
		c.unsubscribe(f.ID, f.Topics)
	default:
		c.reply(f.ID, nil, Errf(CodeBadRequest, "unknown frame type %q", f.Type))
	}
}

// safeRun turns a handler's panic into an internal error, so one bad
// handler cannot take the daemon (and the radio) down with it.
func (c *Conn) safeRun(name string, h CommandFunc, args json.RawMessage) (data any, cerr *CmdError) {
	defer func() {
		if p := recover(); p != nil {
			c.rt.wsLog.Error("cmd "+name+": the command panicked", "conn", c.ID, "panic", fmt.Sprint(p))
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
