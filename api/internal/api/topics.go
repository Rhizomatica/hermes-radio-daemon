package api

import (
	"bytes"
	"encoding/json/jsontext"
	"encoding/json/v2"
	"errors"
	"fmt"
	"slices"
	"sort"
	"time"

	"github.com/Rhizomatica/hermes-radio-daemon/api/spec"
)

// hermes.v1 topics (api-ws.yaml, "Rules"): a client subscribes, gets a
// snapshot of the topic's whole state (seq 0, snapshot: true), and then
// events carrying only the fields that changed, at most 10 a second per
// topic. seq counts the events of a topic on one connection.
//
// A topic is registered with a StateFunc that returns its current state.
// One publisher goroutine reads each topic that has subscribers every
// publishPeriod, through the radiod C API, and compares the top-level
// fields with what each subscriber last received. radiod's own threads
// never call into Go: the API layer reads, C does not push.

// StateFunc returns a topic's current state: a struct or map that marshals
// to a JSON object (the event's data). An error means the topic has no state
// now (mercury is not running, say): it is left out of hello, a subscribe to
// it is refused with not_supported, and its subscribers get no events until
// it has state again.
type StateFunc func() (any, error)

// ConnStateFunc returns a topic's state for one connection, for topics whose
// data depends on what the client asked for (radio.rig: the controls it
// watches). With c == nil it reports whether the topic is available at all:
// an error leaves it out of hello.
type ConnStateFunc func(c *Conn) (any, error)

// StreamFunc reads a stream topic (modem.chat): the items after cursor
// after, oldest first, each one event's data, and the cursor after the last.
// after 0 is the snapshot: the stored history, as much as the topic offers.
// An error means the topic has no state now, as for a StateFunc.
type StreamFunc func(after uint64) (items []any, next uint64, err error)

// BinaryFunc subscribes (on) or unsubscribes connection conn to a topic
// whose events are binary frames radiod's C server produces and sends
// itself (api-ws.yaml's binary frames: spectrum). Go only decides who may
// subscribe, and tells C.
type BinaryFunc func(conn int64, on bool) error

type topic struct {
	role   spec.Role
	state  ConnStateFunc
	each   bool       // per connection: read for each subscriber
	stream StreamFunc // a stream topic: items, not state
	binary BinaryFunc // a binary topic: C sends the frames
}

// subscription is one topic on one connection: what it last received, or
// for a stream topic how far it has got.
type subscription struct {
	seq    uint64
	last   map[string]jsontext.Value
	cursor uint64
}

const publishPeriod = 100 * time.Millisecond // 10 events a second at most

// Topic registers a hermes.v1 topic, readable by role and above. It panics
// if api-ws.yaml has no such topic, or on a second registration.
func (rt *Router) Topic(name string, role spec.Role, state StateFunc) {
	rt.topic(name, topic{role: role, state: func(*Conn) (any, error) { return state() }})
}

// topic registers t as name. It panics if api-ws.yaml has no such topic,
// or on a second registration.
func (rt *Router) topic(name string, t topic) {
	if !spec.HasTopic(name) {
		panic(fmt.Sprintf("api: no topic %q in api-ws.yaml", name))
	}
	if _, dup := rt.topics[name]; dup {
		panic(fmt.Sprintf("api: topic %s registered twice", name))
	}
	rt.topics[name] = t
}

// TopicConn registers a topic whose state is per connection. Its function
// is read once per subscriber each publishing cycle, so it should return
// what the domain keeps (a cache), not do slow I/O itself.
func (rt *Router) TopicConn(name string, role spec.Role, state ConnStateFunc) {
	rt.topic(name, topic{role: role, state: state, each: true})
}

// Stream registers a stream topic, readable by role and above: one event
// per item, never a diff. Subscribing sends the stored history as events
// marked snapshot (seq 0, 1, ...), and then each new item as it comes.
func (rt *Router) Stream(name string, role spec.Role, read StreamFunc) {
	rt.topic(name, topic{role: role, stream: read, state: func(*Conn) (any, error) {
		_, _, err := read(^uint64(0)) // available: nothing after the end
		return struct{}{}, err
	}})
}

// Binary registers a topic of binary frames, readable by role and above:
// subscribing calls sub(conn, true) and sends no snapshot (the frames start
// coming from C); unsubscribing calls sub(conn, false). C drops a closed
// connection's subscriptions itself. C's slots are the station's, so one
// user holds at most BinaryPerUser of them per topic.
func (rt *Router) Binary(name string, role spec.Role, sub BinaryFunc) {
	rt.topic(name, topic{role: role, binary: sub, state: func(*Conn) (any, error) { return struct{}{}, nil }})
}

// BinaryPerUser is how many of one user's connections may hold a binary
// topic at once. C has a fixed number of slots for the whole station (16
// for each spectrum), and without this one user opening sockets could
// take them all.
const BinaryPerUser = 4

// binaryHolders counts user's connections other than except that hold
// topic name.
func (rt *Router) binaryHolders(user int64, name string, except int64) int {
	n := 0
	rt.conns.Range(func(_, v any) bool {
		o := v.(*Conn)
		if o.ID != except && o.User == user {
			o.mu.Lock()
			_, held := o.subs[name]
			o.mu.Unlock()
			if held {
				n++
			}
		}
		return true
	})
	return n
}

// Topics lists the topics a role may subscribe to now, sorted: registered,
// allowed for the role, and with state.
func (rt *Router) Topics(role spec.Role) []string {
	out := []string{}
	for name, t := range rt.topics {
		if !AtLeast(role, t.role) {
			continue
		}
		if _, err := t.state(nil); err == nil {
			out = append(out, name)
		}
	}
	sort.Strings(out)
	return out
}

// event is api-ws.yaml EventEnvelope.
type event struct {
	Type     string                    `json:"type"`
	Topic    string                    `json:"topic"`
	Seq      uint64                    `json:"seq"`
	TS       string                    `json:"ts"`
	Snapshot bool                      `json:"snapshot,omitzero"`
	Data     map[string]jsontext.Value `json:"data"`
}

func stamp(t time.Time) string { return t.UTC().Format("2006-01-02T15:04:05.000Z") }

// fields reads a topic's state (for connection c) as its top-level JSON fields.
func fields(t topic, c *Conn) (map[string]jsontext.Value, error) {
	v, err := t.state(c)
	if err != nil {
		return nil, err
	}
	b, err := json.Marshal(v, output)
	if err != nil {
		return nil, err
	}
	var m map[string]jsontext.Value
	if err := json.Unmarshal(b, &m); err != nil {
		return nil, fmt.Errorf("topic state is not a JSON object: %w", err)
	}
	return m, nil
}

// subscribe handles a subscribe frame on the connection's goroutine: every
// topic is checked, and its state read, before any is subscribed; then one
// snapshot per topic, then the reply, so the client knows the snapshots are
// complete (api-ws.yaml, "Rules"). The publisher sends nothing in between.
func (c *Conn) subscribe(id string, names []string) {
	if len(names) == 0 {
		c.reply(id, nil, Errf(CodeMissingArg, "topics"))
		return
	}
	snaps := make([]map[string]jsontext.Value, len(names))
	type history struct {
		items []map[string]jsontext.Value
		next  uint64
	}
	hists := make([]*history, len(names))
	for i, name := range names {
		t, ok := c.rt.topics[name]
		switch {
		case !spec.HasTopic(name):
			c.reply(id, nil, Errf(CodeBadRequest, "no topic %q", name))
			return
		case !ok:
			c.reply(id, nil, Errf(CodeNotSupported, "this station does not serve %s", name))
			return
		case !AtLeast(c.Role, t.role):
			c.reply(id, nil, Errf(CodeForbidden, "%s needs the %s role", name, t.role))
			return
		}
		if t.binary != nil {
			continue // subscribed in C below, once every topic has passed
		}
		if t.stream != nil {
			items, next, err := t.stream(0)
			if err == nil {
				var h history
				h.next = next
				for _, it := range items {
					var m map[string]jsontext.Value
					if m, err = object(it); err != nil {
						break
					}
					h.items = append(h.items, m)
				}
				hists[i] = &h
			}
			if err != nil {
				c.reply(id, nil, Errf(CodeNotSupported, "%s: %v", name, err))
				return
			}
			continue
		}
		m, err := fields(t, c)
		if err != nil {
			c.reply(id, nil, Errf(CodeNotSupported, "%s: %v", name, err))
			return
		}
		snaps[i] = m
	}
	if cerr := c.subscribeBinary(names); cerr != nil {
		c.reply(id, nil, cerr)
		return
	}
	c.mu.Lock()
	defer c.mu.Unlock()
	for i, name := range names {
		if c.rt.topics[name].binary != nil {
			c.subs[name] = &subscription{} // frames come from C
			continue
		}
		if h := hists[i]; h != nil {
			c.streamSnapshot(name, h.items, h.next)
			continue
		}
		c.snapshot(name, snaps[i])
	}
	c.reply(id, nil, nil)
}

// subscribeBinary subscribes the connection in C to the binary topics among
// names it does not hold yet. If one fails it undoes the others it took, and
// leaves the ones the connection already held as they were.
func (c *Conn) subscribeBinary(names []string) *CmdError {
	var took []string
	undo := func() {
		for _, name := range took {
			_ = c.rt.topics[name].binary(c.ID, false)
		}
	}
	for _, name := range names {
		t := c.rt.topics[name]
		c.mu.Lock()
		_, held := c.subs[name]
		c.mu.Unlock()
		if t.binary == nil || held || slices.Contains(took, name) {
			continue
		}
		if c.rt.binaryHolders(c.User, name, c.ID) >= BinaryPerUser {
			undo()
			return Errf(CodeBusy, "%s: this user already has %d connections on it", name, BinaryPerUser)
		}
		if err := t.binary(c.ID, true); err != nil {
			undo()
			return binaryErr(name, err)
		}
		took = append(took, name)
	}
	return nil
}

// object marshals v as a JSON object's fields.
func object(v any) (map[string]jsontext.Value, error) {
	b, err := json.Marshal(v, output)
	if err != nil {
		return nil, err
	}
	var m map[string]jsontext.Value
	if err := json.Unmarshal(b, &m); err != nil {
		return nil, fmt.Errorf("topic item is not a JSON object: %w", err)
	}
	return m, nil
}

// streamSnapshot (re)starts a stream subscription: the history, one event
// per item from seq 0, all marked snapshot; then new items from next. With
// c.mu held.
func (c *Conn) streamSnapshot(name string, items []map[string]jsontext.Value, next uint64) {
	s := &subscription{cursor: next}
	c.subs[name] = s
	now := stamp(time.Now())
	for i, m := range items {
		if i > 0 {
			s.seq++
		}
		c.send(event{Type: "event", Topic: name, Seq: s.seq, TS: now, Snapshot: true, Data: m})
	}
	if len(items) > 0 {
		s.seq++ // the next event's
	}
}

// snapshot (re)starts a subscription: the whole state m, seq 0. With c.mu
// held.
func (c *Conn) snapshot(name string, m map[string]jsontext.Value) {
	c.subs[name] = &subscription{seq: 0, last: m}
	c.send(event{Type: "event", Topic: name, Seq: 0, TS: stamp(time.Now()), Snapshot: true, Data: m})
}

func (c *Conn) unsubscribe(id string, names []string) {
	if len(names) == 0 {
		c.reply(id, nil, Errf(CodeMissingArg, "topics"))
		return
	}
	c.mu.Lock()
	for _, name := range names {
		if _, subscribed := c.subs[name]; subscribed {
			if t := c.rt.topics[name]; t.binary != nil {
				_ = t.binary(c.ID, false)
			}
		}
		delete(c.subs, name)
	}
	c.mu.Unlock()
	c.reply(id, nil, nil)
}

// binaryErr is a binary topic's refusal: the radio has no such frames, or C
// has no slot left.
func binaryErr(name string, err error) *CmdError {
	if errors.Is(err, ErrBinaryBusy) {
		return Errf(CodeBusy, "%s: too many subscribers", name)
	}
	return Errf(CodeNotSupported, "%s: %v", name, err)
}

// ErrBinaryBusy is what a BinaryFunc returns when C has no subscription slot.
var ErrBinaryBusy = errors.New("no subscription slot left")

// publish runs for the router's lifetime once a connection has opened.
func (rt *Router) publish() {
	t := time.NewTicker(publishPeriod)
	defer t.Stop()
	for now := range t.C {
		rt.publishOnce(now)
	}
}

// publishOnce reads every topic that has a subscriber once, and sends each
// subscriber the fields that changed since its last event.
func (rt *Router) publishOnce(now time.Time) {
	wanted := map[string][]*Conn{}
	rt.conns.Range(func(_, v any) bool {
		c := v.(*Conn)
		c.mu.Lock()
		for name := range c.subs {
			wanted[name] = append(wanted[name], c)
		}
		c.mu.Unlock()
		return true
	})
	for name, conns := range wanted {
		t := rt.topics[name]
		if t.binary != nil {
			continue // C sends these frames
		}
		if t.stream != nil {
			rt.publishStream(name, t, conns, now)
			continue
		}
		var shared map[string]jsontext.Value
		if !t.each {
			var err error
			if shared, err = fields(t, nil); err != nil {
				continue
			}
		}
		for _, c := range conns {
			m := shared
			if t.each {
				var err error
				if m, err = fields(t, c); err != nil {
					continue
				}
			}
			c.mu.Lock()
			if s, ok := c.subs[name]; ok {
				if diff := changed(s.last, m); len(diff) > 0 {
					s.seq++
					s.last = m
					c.send(event{Type: "event", Topic: name, Seq: s.seq, TS: stamp(now), Data: diff})
				}
			}
			c.mu.Unlock()
		}
	}
}

// publishStream sends each subscriber of a stream topic the items after its
// cursor: one read per cursor, as subscribers that keep up share one.
func (rt *Router) publishStream(name string, t topic, conns []*Conn, now time.Time) {
	type batch struct {
		items []map[string]jsontext.Value
		next  uint64
		err   error
	}
	reads := map[uint64]*batch{}
	for _, c := range conns {
		c.mu.Lock()
		s, ok := c.subs[name]
		var after uint64
		if ok {
			after = s.cursor
		}
		c.mu.Unlock()
		if !ok {
			continue
		}
		b := reads[after]
		if b == nil {
			b = &batch{}
			var items []any
			items, b.next, b.err = t.stream(after)
			for _, it := range items {
				m, err := object(it)
				if err != nil {
					b.err = err
					break
				}
				b.items = append(b.items, m)
			}
			reads[after] = b
		}
		if b.err != nil || len(b.items) == 0 {
			continue
		}
		c.mu.Lock()
		if s, ok := c.subs[name]; ok && s.cursor == after {
			for _, m := range b.items {
				c.send(event{Type: "event", Topic: name, Seq: s.seq, TS: stamp(now), Data: m})
				s.seq++
			}
			s.cursor = b.next
		}
		c.mu.Unlock()
	}
}

// changed returns the fields of cur that differ from (or are not in) last.
// A field that is a JSON object in both is compared inside, so an event
// carries only the entries that changed ({"levels":{"AF":0.6}}).
func changed(last, cur map[string]jsontext.Value) map[string]jsontext.Value {
	diff := map[string]jsontext.Value{}
	for k, v := range cur {
		old, ok := last[k]
		if ok && bytes.Equal(old, v) {
			continue
		}
		var o, n map[string]jsontext.Value
		if ok && isObject(old) && isObject(v) && json.Unmarshal(old, &o) == nil && json.Unmarshal(v, &n) == nil {
			if d := changed(o, n); len(d) > 0 {
				b, _ := json.Marshal(d, output)
				diff[k] = b
			}
			continue
		}
		diff[k] = v
	}
	return diff
}

func isObject(b jsontext.Value) bool {
	b = bytes.TrimSpace(b)
	return len(b) > 0 && b[0] == '{'
}
