// Package ale serves ALE 2G (api-rest.yaml tag ale; hermes.v1 ale.*):
// its state, its events, and the operator's actions (listening, scanning,
// calls, accept, reject, terminate, sounding). radiod's modem/ale_link.c
// does the work; this package validates and answers.
package ale

import (
	"context"
	"encoding/json/jsontext"
	"errors"
	"net/http"
	"regexp"
	"time"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/spec"
)

// eventsMax is radiod's ring (ALE_LINK_EVENTS_MAX): as much as one read gives.
const eventsMax = 256

// addressRe is an ALE address: 1-15 of the basic set (A.5.2.4.2).
var addressRe = regexp.MustCompile(`^[A-Z0-9@?]{1,15}$`)

// amdMax is AMD's length (A.5.7.2): at most 90 characters of the expanded set.
const amdMax = 90

// State is AleState (api-rest.yaml).
type State struct {
	Running        bool    `json:"running"`
	Mode           string  `json:"mode"`
	State          string  `json:"state"`
	Self           string  `json:"self"`
	Peer           *string `json:"peer"`
	FrequencyHz    uint32  `json:"frequency_hz"`
	Sideband       string  `json:"sideband"`
	Transmitting   bool    `json:"transmitting"`
	LinkSeconds    uint32  `json:"link_seconds"`
	Votes          int     `json:"votes"`
	Channels       int     `json:"channels"`
	Owner          string  `json:"owner"`
	AutoRespond    bool    `json:"auto_respond"`
	SamplesDropped uint64  `json:"samples_dropped"`
}

// Event is AleEvent (api-rest.yaml).
type Event struct {
	ID          uint64  `json:"id"`
	At          string  `json:"at"`
	Kind        string  `json:"kind"`
	Peer        *string `json:"peer"`
	Text        *string `json:"text"`
	FrequencyHz *uint32 `json:"frequency_hz"`
	Receiver    *string `json:"receiver"`
	Combined    *int    `json:"combined"`
	Preamble    *string `json:"preamble"`
	Unanimous   *int    `json:"unanimous"`
}

func str(s string) *string {
	if s == "" {
		return nil
	}
	return &s
}

func stateOf(s rad.AleState) State {
	out := State{
		Running: s.Running, Mode: s.Mode, State: s.State, Self: s.Self, Peer: str(s.Peer),
		FrequencyHz: s.FrequencyHz, Sideband: "LSB", Transmitting: s.Transmitting,
		LinkSeconds: s.LinkSeconds, Votes: s.Votes, Channels: s.Channels, Owner: s.Owner,
		AutoRespond: s.AutoRespond, SamplesDropped: s.SamplesDropped,
	}
	if s.USB {
		out.Sideband = "USB"
	}
	if out.Mode == "" {
		out.Mode = "monitor"
	}
	if out.Owner == "" {
		out.Owner = "none"
	}
	if out.State == "" {
		out.State = "OFF"
	}
	return out
}

func eventOf(e rad.AleEvent) Event {
	out := Event{
		ID:   e.Seq,
		At:   time.UnixMilli(e.TimeMs).UTC().Format(time.RFC3339Nano),
		Kind: e.Kind,
		Peer: str(e.Peer),
		Text: str(e.Text),
	}
	if e.FrequencyHz != 0 {
		hz := e.FrequencyHz
		out.FrequencyHz = &hz
	}
	if e.Kind == "word" {
		recv, un := "stock", e.Unanimous
		if e.Combined > 0 {
			recv = "combining"
			k := e.Combined
			out.Combined = &k
		}
		out.Receiver, out.Unanimous, out.Preamble = &recv, &un, str(e.Preamble)
	}
	return out
}

// Register adds ALE's operations, commands and topics.
func Register(rt *api.Router) {
	h := handlers{rt.Radio()}
	rt.Op("getAleState", h.getState)
	rt.Op("getAleEvents", h.getEvents)
	rt.Op("startAle", h.rest(h.start))
	rt.Op("stopAle", h.rest(func(jsontext.Value) error { return h.r.AleStop() }))
	rt.Op("callAle", h.rest(h.call))
	rt.Op("acceptAle", h.rest(func(jsontext.Value) error { return h.r.AleAccept() }))
	rt.Op("rejectAle", h.rest(func(jsontext.Value) error { return h.r.AleReject() }))
	rt.Op("terminateAle", h.rest(func(jsontext.Value) error { return h.r.AleTerminate() }))
	rt.Op("soundAle", h.rest(func(jsontext.Value) error { return h.r.AleSound() }))

	rt.Command("ale.start", spec.RoleOperator, h.cmd(h.start))
	rt.Command("ale.stop", spec.RoleOperator, h.cmd(func(jsontext.Value) error { return h.r.AleStop() }))
	rt.Command("ale.call", spec.RoleOperator, h.cmd(h.call))
	rt.Command("ale.accept", spec.RoleOperator, h.cmd(func(jsontext.Value) error { return h.r.AleAccept() }))
	rt.Command("ale.reject", spec.RoleOperator, h.cmd(func(jsontext.Value) error { return h.r.AleReject() }))
	rt.Command("ale.terminate", spec.RoleOperator, h.cmd(func(jsontext.Value) error { return h.r.AleTerminate() }))
	rt.Command("ale.sound", spec.RoleOperator, h.cmd(func(jsontext.Value) error { return h.r.AleSound() }))

	rt.Topic("ale.state", spec.RoleReadonly, h.topicState)
	rt.Stream("ale.events", spec.RoleReadonly, h.stream)
}

type handlers struct{ r rad.Radio }

// errNotRunning: radiod has no ALE (not built in, or ale:enable_ale off).
var errNotRunning = errors.New("ALE is not running in this daemon")

// fieldError is a request field that failed validation.
type fieldError struct{ field, msg string }

func (e fieldError) Error() string { return e.field + ": " + e.msg }

// ── one validator for each REST body and its command ──

func (h handlers) start(args jsontext.Value) error {
	var a struct {
		Mode *string `json:"mode"`
	}
	if err := decode(args, &a); err != nil {
		return err
	}
	if a.Mode == nil {
		return fieldError{"mode", "required"}
	}
	if *a.Mode != "listen" && *a.Mode != "scan" {
		return fieldError{"mode", "listen or scan"}
	}
	return h.r.AleStart(*a.Mode)
}

func (h handlers) call(args jsontext.Value) error {
	var a struct {
		To  *string `json:"to"`
		Amd *string `json:"amd"`
	}
	if err := decode(args, &a); err != nil {
		return err
	}
	if a.To == nil {
		return fieldError{"to", "required"}
	}
	if !addressRe.MatchString(*a.To) {
		return fieldError{"to", "an ALE address: 1 to 15 of A-Z 0-9 @ ?"}
	}
	amd := ""
	if a.Amd != nil {
		amd = *a.Amd
		if len(amd) > amdMax {
			return fieldError{"amd", "at most 90 characters"}
		}
		for i := 0; i < len(amd); i++ {
			if amd[i] < 0x20 || amd[i] > 0x5f {
				return fieldError{"amd", "the expanded set: space to underscore (0x20-0x5F), so upper case"}
			}
		}
	}
	return h.r.AleCall(*a.To, amd)
}

// errDecode wraps a body that is not the arguments' shape.
type errDecode struct{ cerr *api.CmdError }

func (e errDecode) Error() string { return e.cerr.Message }

func decode(args jsontext.Value, dst any) error {
	if len(args) == 0 {
		args = jsontext.Value("{}")
	}
	if cerr := api.DecodeArgs(args, dst); cerr != nil {
		return errDecode{cerr}
	}
	return nil
}

// ── REST ──

func (h handlers) getState(w http.ResponseWriter, r *http.Request) {
	api.JSON(w, http.StatusOK, stateOf(h.r.AleState()))
}

func (h handlers) getEvents(w http.ResponseWriter, r *http.Request) {
	limit, ok := api.Limit(w, r)
	if !ok {
		return
	}
	after, ok := api.DecodeCursor(w, r) // oldest first: events with id > cursor
	if !ok {
		return
	}
	page := api.Page[Event]{Data: []Event{}}
	evs, _, err := h.r.AleEvents(after, eventsMax)
	if err != nil && !errors.Is(err, rad.ErrNotSupported) {
		api.Problem(w, r, http.StatusInternalServerError, "internal", err.Error(), nil)
		return
	}
	for _, e := range evs {
		if len(page.Data) == limit {
			page.NextCursor = api.EncodeCursor(page.Data[len(page.Data)-1].ID)
			break
		}
		page.Data = append(page.Data, eventOf(e))
	}
	api.JSON(w, http.StatusOK, page)
}

// rest runs an action with the request's JSON body (if any) and answers
// with ALE's state, or the problem.
func (h handlers) rest(do func(jsontext.Value) error) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		var body jsontext.Value
		if r.ContentLength != 0 && !api.Decode(w, r, &body) {
			return
		}
		err := do(body)
		var fe fieldError
		var de errDecode
		switch {
		case err == nil:
			api.JSON(w, http.StatusOK, stateOf(h.r.AleState()))
		case errors.As(err, &fe):
			f := api.Fields{}
			f.Add(fe.field, fe.msg)
			api.Invalid(w, r, f)
		case errors.As(err, &de):
			api.Problem(w, r, http.StatusUnprocessableEntity, "invalid", de.Error(), nil)
		case errors.Is(err, rad.ErrBusy):
			api.Problem(w, r, http.StatusConflict, "busy", "ALE's state does not allow it now", nil)
		case errors.Is(err, rad.ErrInvalid):
			api.Problem(w, r, http.StatusUnprocessableEntity, "invalid",
				"no ale:self_address, no channels to scan, or a value ALE does not take", nil)
		case errors.Is(err, rad.ErrNotSupported):
			api.Problem(w, r, http.StatusServiceUnavailable, "not_running", errNotRunning.Error(), nil)
		default:
			api.Problem(w, r, http.StatusInternalServerError, "internal", err.Error(), nil)
		}
	}
}

// ── hermes.v1 ──

func (h handlers) cmd(do func(jsontext.Value) error) api.CommandFunc {
	return func(_ context.Context, _ *api.Conn, args jsontext.Value) (any, *api.CmdError) {
		err := do(args)
		var fe fieldError
		var de errDecode
		switch {
		case err == nil:
			return stateOf(h.r.AleState()), nil
		case errors.As(err, &de):
			return nil, de.cerr
		case errors.As(err, &fe):
			if fe.msg == "required" {
				return nil, api.Errf(api.CodeMissingArg, "%s", fe.field)
			}
			return nil, api.Errf(api.CodeOutOfRange, "%s", fe.Error())
		case errors.Is(err, rad.ErrBusy):
			return nil, api.Errf(api.CodeBusy, "ALE's state does not allow it now")
		case errors.Is(err, rad.ErrInvalid):
			return nil, api.Errf(api.CodeOutOfRange, "no ale:self_address, no channels to scan, or a value ALE does not take")
		case errors.Is(err, rad.ErrNotSupported):
			return nil, api.Errf(api.CodeNotSupported, "%v", errNotRunning)
		default:
			return nil, api.Errf(api.CodeInternal, "%v", err)
		}
	}
}

func (h handlers) topicState() (any, error) {
	st := h.r.AleState()
	if !st.Running {
		return nil, errNotRunning
	}
	return stateOf(st), nil
}

func (h handlers) stream(after uint64) ([]any, uint64, error) {
	evs, last, err := h.r.AleEvents(after, eventsMax)
	if err != nil {
		return nil, after, err
	}
	items := make([]any, 0, len(evs))
	next := after
	for _, e := range evs {
		items = append(items, eventOf(e))
		next = e.Seq
	}
	if len(evs) == 0 && last > after {
		next = last
	}
	return items, next, nil
}
