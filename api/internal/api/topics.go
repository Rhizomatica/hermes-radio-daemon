package api

import (
	"bytes"
	"encoding/json"
	"fmt"
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

type topic struct {
	role   spec.Role
	state  ConnStateFunc
	each   bool       // per connection: read for each subscriber
	stream StreamFunc // a stream topic: items, not state
}

// subscription is one topic on one connection: what it last received, or
// for a stream topic how far it has got.
type subscription struct {
	seq    uint64
	last   map[string]json.RawMessage
	cursor uint64
}

const publishPeriod = 100 * time.Millisecond // 10 events a second at most

// Topic registers a hermes.v1 topic, readable by role and above. It panics
// if api-ws.yaml has no such topic, or on a second registration.
func (rt *Router) Topic(name string, role spec.Role, state StateFunc) {
	if !spec.HasTopic(name) {
		panic(fmt.Sprintf("api: no topic %q in api-ws.yaml", name))
	}
	if _, dup := rt.topics[name]; dup {
		panic(fmt.Sprintf("api: topic %s registered twice", name))
	}
	rt.topics[name] = topic{role: role, state: func(*Conn) (any, error) { return state() }}
}

// TopicConn registers a topic whose state is per connection. Its function
// is read once per subscriber each publishing cycle, so it should return
// what the domain keeps (a cache), not do slow I/O itself.
func (rt *Router) TopicConn(name string, role spec.Role, state ConnStateFunc) {
	rt.Topic(name, role, func() (any, error) { return nil, nil })
	rt.topics[name] = topic{role: role, state: state, each: true}
}

// Stream registers a stream topic, readable by role and above: one event
// per item, never a diff. Subscribing sends the stored history as events
// marked snapshot (seq 0, 1, ...), and then each new item as it comes.
func (rt *Router) Stream(name string, role spec.Role, read StreamFunc) {
	rt.Topic(name, role, func() (any, error) { return nil, nil })
	rt.topics[name] = topic{role: role, stream: read, state: func(*Conn) (any, error) {
		_, _, err := read(^uint64(0)) // available: nothing after the end
		return struct{}{}, err
	}}
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
	Type     string                     `json:"type"`
	Topic    string                     `json:"topic"`
	Seq      uint64                     `json:"seq"`
	TS       string                     `json:"ts"`
	Snapshot bool                       `json:"snapshot,omitempty"`
	Data     map[string]json.RawMessage `json:"data"`
}

func stamp(t time.Time) string { return t.UTC().Format("2006-01-02T15:04:05.000Z") }

// fields reads a topic's state (for connection c) as its top-level JSON fields.
func fields(t topic, c *Conn) (map[string]json.RawMessage, error) {
	v, err := t.state(c)
	if err != nil {
		return nil, err
	}
	b, err := json.Marshal(v)
	if err != nil {
		return nil, err
	}
	var m map[string]json.RawMessage
	if err := json.Unmarshal(b, &m); err != nil {
		return nil, fmt.Errorf("topic state is not a JSON object: %w", err)
	}
	return m, nil
}

// subscribe handles a subscribe frame on the connection's goroutine: every
// topic is checked, and its state read, before any is subscribed; then the
// reply, then one snapshot per topic.
func (c *Conn) subscribe(id string, names []string) {
	if len(names) == 0 {
		c.reply(id, nil, Errf(CodeMissingArg, "topics"))
		return
	}
	snaps := make([]map[string]json.RawMessage, len(names))
	type history struct {
		items []map[string]json.RawMessage
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
		if t.stream != nil {
			items, next, err := t.stream(0)
			if err == nil {
				var h history
				h.next = next
				for _, it := range items {
					var m map[string]json.RawMessage
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
	c.reply(id, nil, nil)
	for i, name := range names {
		if h := hists[i]; h != nil {
			c.streamSnapshot(name, h.items, h.next)
			continue
		}
		c.snapshot(name, snaps[i])
	}
}

// object marshals v as a JSON object's fields.
func object(v any) (map[string]json.RawMessage, error) {
	b, err := json.Marshal(v)
	if err != nil {
		return nil, err
	}
	var m map[string]json.RawMessage
	if err := json.Unmarshal(b, &m); err != nil {
		return nil, fmt.Errorf("topic item is not a JSON object: %w", err)
	}
	return m, nil
}

// streamSnapshot (re)starts a stream subscription: the history, one event
// per item from seq 0, all marked snapshot; then new items from next.
func (c *Conn) streamSnapshot(name string, items []map[string]json.RawMessage, next uint64) {
	c.mu.Lock()
	defer c.mu.Unlock()
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

// snapshot (re)starts a subscription: the whole state m, seq 0.
func (c *Conn) snapshot(name string, m map[string]json.RawMessage) {
	c.mu.Lock()
	defer c.mu.Unlock()
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
		delete(c.subs, name)
	}
	c.mu.Unlock()
	c.reply(id, nil, nil)
}

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
		if t.stream != nil {
			rt.publishStream(name, t, conns, now)
			continue
		}
		var shared map[string]json.RawMessage
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
		items []map[string]json.RawMessage
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
func changed(last, cur map[string]json.RawMessage) map[string]json.RawMessage {
	diff := map[string]json.RawMessage{}
	for k, v := range cur {
		old, ok := last[k]
		if ok && bytes.Equal(old, v) {
			continue
		}
		var o, n map[string]json.RawMessage
		if ok && isObject(old) && isObject(v) && json.Unmarshal(old, &o) == nil && json.Unmarshal(v, &n) == nil {
			if d := changed(o, n); len(d) > 0 {
				b, _ := json.Marshal(d)
				diff[k] = b
			}
			continue
		}
		diff[k] = v
	}
	return diff
}

func isObject(b json.RawMessage) bool {
	b = bytes.TrimSpace(b)
	return len(b) > 0 && b[0] == '{'
}
