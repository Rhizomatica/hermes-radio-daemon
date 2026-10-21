// The radio's controls on hermes.v1 (api-ws.yaml): levels, functions and
// parameters (AF, RF, NB, PREAMP) in radiod's control vocabulary
// (radio/radio_controls.h), whichever backend drives the radio: the sBitx,
// a CAT rig, or one added later.
//
//   - rig.get reads controls now (one CAT read each);
//   - rig.set sets one;
//   - rig.watch chooses the controls a client's radio.rig events carry, and
//     how often they are read (at least every 250 ms).
//
// Which controls exist is the rig's own capability list, not a fixed set:
// every name is checked against it before the rig is touched. A watch reads
// the rig on its own goroutine, per connection, so slow CAT never holds up
// the publisher; radio.rig is the latest values it read.

package radio

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"sort"
	"sync"
	"time"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/logging"
	"github.com/Rhizomatica/hermes-radio-daemon/api/spec"
)

const (
	maxNames        = 32
	minIntervalMS   = 250
	maxIntervalMS   = 60000
	defaultInterval = 1000
)

// registerControls adds the rig.* commands and the radio.rig topic.
func registerControls(rt *api.Router) {
	h := &controls{r: rt.Radio(), log: logging.New(rt.Radio(), "api.radio"), watches: map[int64]*watch{}}
	rt.Command("rig.get", spec.RoleOperator, h.cmdGet)
	rt.Command("rig.set", spec.RoleOperator, h.cmdSet)
	rt.Command("rig.watch", spec.RoleOperator, h.cmdWatch)
	rt.TopicConn("radio.rig", spec.RoleReadonly, h.state)
}

type controls struct {
	r   rad.Radio
	log *slog.Logger

	mu      sync.Mutex
	watches map[int64]*watch // by connection
}

// Values is the radio.rig event's data, and rig.get's and rig.set's reply.
type Values struct {
	Levels map[string]float64 `json:"levels"`
	Funcs  map[string]bool    `json:"funcs"`
	Parms  map[string]float64 `json:"parms"`
}

func newValues() Values {
	return Values{Levels: map[string]float64{}, Funcs: map[string]bool{}, Parms: map[string]float64{}}
}

func (v Values) put(kind rad.ControlKind, name string, value float64) {
	switch kind {
	case rad.Func:
		v.Funcs[name] = value != 0
	case rad.Parm:
		v.Parms[name] = value
	default:
		v.Levels[name] = value
	}
}

func (v Values) copy() Values {
	out := newValues()
	for k, x := range v.Levels {
		out.Levels[k] = x
	}
	for k, x := range v.Funcs {
		out.Funcs[k] = x
	}
	for k, x := range v.Parms {
		out.Parms[k] = x
	}
	return out
}

// controls is the rig's capability list by name.
func (h *controls) caps() map[string]rad.Control {
	m := map[string]rad.Control{}
	for _, c := range h.r.Controls() {
		m[c.Name] = c
	}
	return m
}

// readable checks names against the rig's controls: all must exist and be
// readable, at most maxNames, no duplicates.
func (h *controls) readable(names []string) *api.CmdError {
	if len(names) > maxNames {
		return api.Errf(api.CodeOutOfRange, "names: at most %d", maxNames)
	}
	caps := h.caps()
	seen := map[string]bool{}
	for _, n := range names {
		c, ok := caps[n]
		switch {
		case !ok:
			return api.Errf(api.CodeNotSupported, "%s: this rig has no such control", n)
		case !c.CanGet:
			return api.Errf(api.CodeNotSupported, "%s: this rig cannot read it", n)
		case seen[n]:
			return api.Errf(api.CodeBadRequest, "%s: named twice", n)
		}
		seen[n] = true
	}
	return nil
}

func ctrlErr(name string, err error) *api.CmdError {
	switch {
	case errors.Is(err, rad.ErrIO):
		return api.Errf(api.CodeRigTimeout, "%s: the rig did not answer", name)
	case errors.Is(err, rad.ErrInvalid):
		return api.Errf(api.CodeOutOfRange, "%s: a value the rig does not take", name)
	case errors.Is(err, rad.ErrBusy):
		return api.Errf(api.CodeBusy, "%s: refused while transmitting", name)
	default:
		return api.Errf(api.CodeNotSupported, "%s: %v", name, err)
	}
}

// read reads names from the rig, one CAT read each.
func (h *controls) read(names []string) (Values, *api.CmdError) {
	v := newValues()
	for _, n := range names {
		kind, x, err := h.r.GetControl(n)
		if err != nil {
			return Values{}, ctrlErr(n, err)
		}
		v.put(kind, n, x)
	}
	return v, nil
}

// rig.get {names}: the values now.
func (h *controls) cmdGet(_ context.Context, _ *api.Conn, args json.RawMessage) (any, *api.CmdError) {
	var a struct {
		Names *[]string `json:"names"`
	}
	if cerr := api.DecodeArgs(args, &a); cerr != nil {
		return nil, cerr
	}
	if a.Names == nil || len(*a.Names) == 0 {
		return nil, api.Errf(api.CodeMissingArg, "names")
	}
	if cerr := h.readable(*a.Names); cerr != nil {
		return nil, cerr
	}
	v, cerr := h.read(*a.Names)
	if cerr != nil {
		return nil, cerr
	}
	return v, nil
}

// rig.set {name, value}: a function takes true or false, a level or a
// parameter a number within the rig's range. Replies with the value set.
func (h *controls) cmdSet(_ context.Context, _ *api.Conn, args json.RawMessage) (any, *api.CmdError) {
	var a struct {
		Name  *string         `json:"name"`
		Value json.RawMessage `json:"value"`
	}
	if cerr := api.DecodeArgs(args, &a); cerr != nil {
		return nil, cerr
	}
	if a.Name == nil || *a.Name == "" {
		return nil, api.Errf(api.CodeMissingArg, "name")
	}
	if len(a.Value) == 0 || string(a.Value) == "null" {
		return nil, api.Errf(api.CodeMissingArg, "value")
	}
	name := *a.Name
	c, ok := h.caps()[name]
	if !ok {
		return nil, api.Errf(api.CodeNotSupported, "%s: this rig has no such control", name)
	}
	if !c.CanSet {
		return nil, api.Errf(api.CodeNotSupported, "%s: this rig cannot set it", name)
	}
	var x float64
	if c.Kind == rad.Func {
		var on bool
		if json.Unmarshal(a.Value, &on) != nil {
			return nil, api.Errf(api.CodeBadRequest, "%s is on/off: value is true or false", name)
		}
		if on {
			x = 1
		}
	} else {
		if json.Unmarshal(a.Value, &x) != nil {
			return nil, api.Errf(api.CodeBadRequest, "%s: value is a number", name)
		}
		if c.Max > c.Min && (x < c.Min || x > c.Max) {
			return nil, api.Errf(api.CodeOutOfRange, "%s: from %g to %g", name, c.Min, c.Max)
		}
	}
	if err := h.r.SetControl(name, x); err != nil {
		return nil, ctrlErr(name, err)
	}
	v := newValues()
	v.put(c.Kind, name, x)
	return v, nil
}

// watch is one connection's rig.watch: its poller's latest values.
type watch struct {
	names    []string
	interval time.Duration
	stop     chan struct{}

	mu     sync.Mutex
	values Values
	failed bool // the last read failed (logged once until it works again)
}

// rig.watch {names, interval_ms}: radio.rig events for this client carry
// these controls, read every interval_ms. An empty list stops.
func (h *controls) cmdWatch(_ context.Context, c *api.Conn, args json.RawMessage) (any, *api.CmdError) {
	var a struct {
		Names      *[]string `json:"names"`
		IntervalMS *int      `json:"interval_ms"`
	}
	if cerr := api.DecodeArgs(args, &a); cerr != nil {
		return nil, cerr
	}
	if a.Names == nil {
		return nil, api.Errf(api.CodeMissingArg, "names")
	}
	interval := defaultInterval
	if a.IntervalMS != nil {
		interval = *a.IntervalMS
	}
	if interval < minIntervalMS || interval > maxIntervalMS {
		return nil, api.Errf(api.CodeOutOfRange, "interval_ms: from %d to %d", minIntervalMS, maxIntervalMS)
	}
	names := append([]string(nil), *a.Names...)
	if cerr := h.readable(names); cerr != nil {
		return nil, cerr
	}
	sort.Strings(names)

	h.mu.Lock()
	if old := h.watches[c.ID]; old != nil {
		close(old.stop)
		delete(h.watches, c.ID)
	}
	if len(names) > 0 {
		w := &watch{names: names, interval: time.Duration(interval) * time.Millisecond,
			stop: make(chan struct{}), values: newValues()}
		h.watches[c.ID] = w
		go h.poll(c, w)
	}
	h.mu.Unlock()
	return map[string]any{"names": names, "interval_ms": interval}, nil
}

// poll reads a watch's controls every interval until it is replaced,
// stopped, or its connection closes.
func (h *controls) poll(c *api.Conn, w *watch) {
	defer func() {
		h.mu.Lock()
		if h.watches[c.ID] == w {
			delete(h.watches, c.ID)
		}
		h.mu.Unlock()
	}()
	t := time.NewTicker(w.interval)
	defer t.Stop()
	for {
		v, cerr := h.read(w.names)
		w.mu.Lock()
		switch {
		case cerr == nil:
			w.values, w.failed = v, false
		case !w.failed:
			w.failed = true
			h.log.Warn("rig.watch: reading the rig failed; keeping the last values",
				"conn", c.ID, "error", cerr.Message)
		}
		w.mu.Unlock()
		select {
		case <-t.C:
		case <-w.stop:
			return
		case <-c.Done():
			return
		}
	}
}

// state is radio.rig for one connection: its watch's latest values, empty
// without a watch. With c == nil it tells whether the topic is offered: a
// rig with controls.
func (h *controls) state(c *api.Conn) (any, error) {
	if c == nil {
		if len(h.r.Controls()) == 0 {
			return nil, fmt.Errorf("this radio has no rig controls")
		}
		return nil, nil
	}
	h.mu.Lock()
	w := h.watches[c.ID]
	h.mu.Unlock()
	if w == nil {
		return newValues(), nil
	}
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.values.copy(), nil
}
