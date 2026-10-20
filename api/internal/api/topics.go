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

type topic struct {
	role  spec.Role
	state StateFunc
}

// subscription is one topic on one connection: what it last received.
type subscription struct {
	seq  uint64
	last map[string]json.RawMessage
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
	rt.topics[name] = topic{role, state}
}

// Topics lists the topics a role may subscribe to now, sorted: registered,
// allowed for the role, and with state.
func (rt *Router) Topics(role spec.Role) []string {
	out := []string{}
	for name, t := range rt.topics {
		if !AtLeast(role, t.role) {
			continue
		}
		if _, err := t.state(); err == nil {
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

// fields reads a topic's state as its top-level JSON fields.
func fields(t topic) (map[string]json.RawMessage, error) {
	v, err := t.state()
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
		m, err := fields(t)
		if err != nil {
			c.reply(id, nil, Errf(CodeNotSupported, "%s: %v", name, err))
			return
		}
		snaps[i] = m
	}
	c.reply(id, nil, nil)
	for i, name := range names {
		c.snapshot(name, snaps[i])
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
		m, err := fields(rt.topics[name])
		if err != nil {
			continue
		}
		for _, c := range conns {
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

// changed returns the fields of cur that differ from (or are not in) last.
func changed(last, cur map[string]json.RawMessage) map[string]json.RawMessage {
	diff := map[string]json.RawMessage{}
	for k, v := range cur {
		if old, ok := last[k]; !ok || !bytes.Equal(old, v) {
			diff[k] = v
		}
	}
	return diff
}
