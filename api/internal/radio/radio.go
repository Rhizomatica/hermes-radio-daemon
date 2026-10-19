// Package radio serves the radio's REST operations (api-rest.yaml, tag
// radio) and hermes.v1 commands (radio.set, radio.select_profile,
// radio.reset_protection) over the daemon's facade.
package radio

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"strconv"
	"strings"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/spec"
)

// Register adds the radio's operations and commands.
func Register(rt *api.Router) {
	h := handlers{rt.Radio()}
	rt.Op("getRadioState", h.getState)
	rt.Op("listProfiles", h.listProfiles)
	rt.Op("getProfile", h.getProfile)
	rt.Op("updateProfile", h.updateProfile)
	rt.Op("selectProfile", h.selectProfile)
	rt.Op("resetProtection", h.resetProtection)

	rt.Command("radio.set", spec.RoleOperator, h.cmdSet)
	rt.Command("radio.select_profile", spec.RoleOperator, h.cmdSelect)
	rt.Command("radio.reset_protection", spec.RoleOperator, h.cmdResetProtection)
}

type handlers struct{ r rad.Radio }

// ── wire shapes (api-rest.yaml components) ──

// State is RadioState.
type State struct {
	ActiveProfile int      `json:"active_profile"`
	TX            bool     `json:"tx"`
	FrequencyHz   uint32   `json:"frequency_hz"`
	Mode          string   `json:"mode"`
	Power         int      `json:"power"`
	SWR           *float64 `json:"swr"`
	FwdW          *float64 `json:"fwd_w"`
	RefW          *float64 `json:"ref_w"`
	SMeterDB      *float64 `json:"s_meter_db"`
	Protection    bool     `json:"protection"`
	DigitalVoice  bool     `json:"digital_voice"`
	Backend       string   `json:"backend"`
	RigModel      *string  `json:"rig_model"`
}

// Profile is RadioProfile. radiod keeps step, ref threshold and BFO
// station-wide, so every profile reports the same; it has no profile names
// or CTCSS tones (name and tone_hz are null). It has no transmit timeout
// either: its "timeout" is how long a non-default profile lasts before the
// radio falls back to the default one, which the spec has no field for, so
// timeout_s is left out (it is optional) rather than mislabelled.
type Profile struct {
	Profile       int     `json:"profile"`
	Name          *string `json:"name"`
	FrequencyHz   uint32  `json:"frequency_hz"`
	Mode          string  `json:"mode"`
	Power         int     `json:"power"`
	Volume        int     `json:"volume"`
	StepHz        uint32  `json:"step_hz"`
	FilterWidthHz *uint32 `json:"filter_width_hz"`
	ToneHz        *int    `json:"tone_hz"`
	RefThreshold  uint32  `json:"ref_threshold"`
	BfoHz         int     `json:"bfo_hz"`
	DigitalVoice  bool    `json:"digital_voice"`
}

func stateOf(s rad.State) State {
	out := State{
		ActiveProfile: s.ActiveProfile, TX: s.TX, FrequencyHz: s.FrequencyHz, Mode: s.Mode,
		Power: s.Power, Protection: s.Protection,
		DigitalVoice: s.DigitalVoice, Backend: s.Backend,
	}
	if s.TX { // the bridge readings mean something only while transmitting
		out.SWR, out.FwdW, out.RefW = &s.SWR, &s.FwdW, &s.RefW
	}
	if s.SMeterValid {
		out.SMeterDB = &s.SMeterDB
	}
	return out
}

func profileOf(p rad.Profile) Profile {
	out := Profile{
		Profile: p.Profile, FrequencyHz: p.FrequencyHz, Mode: p.Mode, Power: p.Power,
		Volume: p.Volume, StepHz: p.StepHz, RefThreshold: p.RefThreshold,
		BfoHz: p.BfoHz, DigitalVoice: p.DigitalVoice,
	}
	if p.FilterWidthHz != 0 {
		w := p.FilterWidthHz
		out.FilterWidthHz = &w
	}
	return out
}

// ── REST ──

func (h handlers) getState(w http.ResponseWriter, r *http.Request) {
	api.JSON(w, http.StatusOK, stateOf(h.r.State()))
}

func (h handlers) listProfiles(w http.ResponseWriter, r *http.Request) {
	out := api.Page[Profile]{Data: []Profile{}}
	for i := 0; i < h.r.ProfileCount(); i++ {
		if p, err := h.r.Profile(i); err == nil {
			out.Data = append(out.Data, profileOf(p))
		}
	}
	api.JSON(w, http.StatusOK, out)
}

// profileParam reads {profile}; false after answering 404.
func (h handlers) profileParam(w http.ResponseWriter, r *http.Request) (int, bool) {
	n, err := strconv.Atoi(r.PathValue("profile"))
	if err != nil || n < 0 || n >= h.r.ProfileCount() {
		api.Problem(w, r, http.StatusNotFound, "not_found", "no profile "+r.PathValue("profile"), nil)
		return 0, false
	}
	return n, true
}

func (h handlers) getProfile(w http.ResponseWriter, r *http.Request) {
	n, ok := h.profileParam(w, r)
	if !ok {
		return
	}
	p, err := h.r.Profile(n)
	if err != nil {
		api.Problem(w, r, http.StatusNotFound, "not_found", err.Error(), nil)
		return
	}
	api.JSON(w, http.StatusOK, profileOf(p))
}

func (h handlers) updateProfile(w http.ResponseWriter, r *http.Request) {
	n, ok := h.profileParam(w, r)
	if !ok {
		return
	}
	var raw map[string]json.RawMessage
	if !api.Decode(w, r, &raw) {
		return
	}
	patch, fields := parsePatch(raw, false)
	if !fields.OK() {
		api.Invalid(w, r, fields)
		return
	}
	if err := h.apply(n, patch); err != nil {
		h.problemFor(w, r, err)
		return
	}
	p, _ := h.r.Profile(n)
	api.JSON(w, http.StatusOK, profileOf(p))
}

func (h handlers) selectProfile(w http.ResponseWriter, r *http.Request) {
	var body struct {
		Profile *int `json:"profile"`
	}
	if !api.Decode(w, r, &body) {
		return
	}
	if body.Profile == nil || *body.Profile < 0 || *body.Profile >= h.r.ProfileCount() {
		api.Invalid(w, r, api.Fields{"profile": {fmt.Sprintf("a profile from 0 to %d", h.r.ProfileCount()-1)}})
		return
	}
	if err := h.r.SelectProfile(*body.Profile); err != nil {
		h.problemFor(w, r, err)
		return
	}
	api.JSON(w, http.StatusOK, stateOf(h.r.State()))
}

func (h handlers) resetProtection(w http.ResponseWriter, r *http.Request) {
	h.r.ResetProtection()
	api.JSON(w, http.StatusNoContent, nil)
}

func (h handlers) problemFor(w http.ResponseWriter, r *http.Request, err error) {
	switch {
	case errors.Is(err, rad.ErrBusy):
		api.Problem(w, r, http.StatusConflict, "busy", "refused while transmitting", nil)
	case errors.Is(err, rad.ErrNoProfile):
		api.Problem(w, r, http.StatusNotFound, "not_found", err.Error(), nil)
	case errors.Is(err, rad.ErrInvalid), errors.Is(err, rad.ErrNotSupported):
		api.Problem(w, r, http.StatusUnprocessableEntity, "invalid", err.Error(), nil)
	default:
		api.Problem(w, r, http.StatusInternalServerError, "internal", err.Error(), nil)
	}
}

// ── one validator for PATCH and radio.set ──

type patch struct {
	profile                         *int
	frequency                       *uint32
	mode                            *string
	power, volume, bfo              *int
	step, refThreshold, filterWidth *uint32
	digitalVoice                    *bool
}

var modes = map[string]bool{"LSB": true, "USB": true, "CW": true, "FM": true, "AM": true,
	"DRM": true, "FT8": true, "RTTY": true, "DSTAR": true}

// parsePatch checks every field before anything is applied. withProfile
// allows the "profile" key (radio.set). Unknown keys fail.
func parsePatch(raw map[string]json.RawMessage, withProfile bool) (patch, api.Fields) {
	var p patch
	f := api.Fields{}
	intIn := func(key string, lo, hi int64) *int64 {
		var v int64
		if err := json.Unmarshal(raw[key], &v); err != nil {
			f.Add(key, "an integer")
			return nil
		}
		if v < lo || v > hi {
			f.Add(key, fmt.Sprintf("from %d to %d", lo, hi))
			return nil
		}
		return &v
	}
	null := func(key string) bool { return strings.TrimSpace(string(raw[key])) == "null" }
	for key := range raw {
		switch key {
		case "profile":
			if !withProfile {
				f.Add(key, "not a field of a profile")
			} else if v := intIn(key, 0, 1<<16); v != nil {
				n := int(*v)
				p.profile = &n
			}
		case "frequency_hz":
			if v := intIn(key, 0, 1<<32-1); v != nil {
				hz := uint32(*v)
				p.frequency = &hz
			}
		case "mode":
			var m string
			if json.Unmarshal(raw[key], &m) != nil || !modes[strings.ToUpper(m)] {
				f.Add(key, "one of LSB, USB, CW, FM, AM, DRM, FT8, RTTY, DSTAR")
			} else {
				m = strings.ToUpper(m)
				p.mode = &m
			}
		case "power", "volume":
			if v := intIn(key, 0, 100); v != nil {
				n := int(*v)
				if key == "power" {
					p.power = &n
				} else {
					p.volume = &n
				}
			}
		case "step_hz":
			if v := intIn(key, 1, 1<<32-1); v != nil {
				n := uint32(*v)
				p.step = &n
			}
		case "timeout_s":
			f.Add(key, "radiod has no transmit timeout (its profile timeout is the fallback to the default profile)")
		case "ref_threshold":
			if v := intIn(key, 0, 1<<32-1); v != nil {
				n := uint32(*v)
				p.refThreshold = &n
			}
		case "bfo_hz":
			if v := intIn(key, -(1 << 31), 1<<31-1); v != nil {
				n := int(*v)
				p.bfo = &n
			}
		case "filter_width_hz":
			var n uint32 // null: the rig's default
			if !null(key) {
				v := intIn(key, 0, 1<<32-1)
				if v == nil {
					continue
				}
				n = uint32(*v)
			}
			p.filterWidth = &n
		case "digital_voice":
			var b bool
			if json.Unmarshal(raw[key], &b) != nil {
				f.Add(key, "true or false")
			} else {
				p.digitalVoice = &b
			}
		case "name":
			if !null(key) {
				f.Add(key, "radiod does not store profile names")
			}
		case "tone_hz":
			if !null(key) {
				f.Add(key, "this radio has no CTCSS tone")
			}
		default:
			f.Add(key, "unknown field")
		}
	}
	return p, f
}

// apply sets every field of a checked patch. Digital voice is refused while
// transmitting; it is checked first, so a refusal applies nothing.
func (h handlers) apply(n int, p patch) error {
	if p.digitalVoice != nil && h.r.State().TX {
		return rad.ErrBusy
	}
	steps := []func() error{}
	if p.mode != nil {
		steps = append(steps, func() error { return h.r.SetMode(n, *p.mode) })
	}
	if p.frequency != nil {
		steps = append(steps, func() error { return h.r.SetFrequency(n, *p.frequency) })
	}
	if p.power != nil {
		steps = append(steps, func() error { return h.r.SetPower(n, *p.power) })
	}
	if p.volume != nil {
		steps = append(steps, func() error { return h.r.SetVolume(n, *p.volume) })
	}
	if p.filterWidth != nil {
		steps = append(steps, func() error { return h.r.SetFilterWidth(n, *p.filterWidth) })
	}
	if p.step != nil {
		steps = append(steps, func() error { return h.r.SetStep(*p.step) })
	}
	if p.refThreshold != nil {
		steps = append(steps, func() error { return h.r.SetRefThreshold(*p.refThreshold) })
	}
	if p.bfo != nil {
		steps = append(steps, func() error { return h.r.SetBfo(*p.bfo) })
	}
	if p.digitalVoice != nil {
		steps = append(steps, func() error { return h.r.SetDigitalVoice(n, *p.digitalVoice) })
	}
	for _, s := range steps {
		if err := s(); err != nil {
			return err
		}
	}
	return nil
}

// ── hermes.v1 ──

func cmdErr(err error) *api.CmdError {
	switch {
	case errors.Is(err, rad.ErrBusy):
		return api.Errf(api.CodeBusy, "refused while transmitting")
	case errors.Is(err, rad.ErrNoProfile):
		return api.Errf(api.CodeOutOfRange, "no such profile")
	case errors.Is(err, rad.ErrNotSupported):
		return api.Errf(api.CodeNotSupported, "%v", err)
	default:
		return api.Errf(api.CodeOutOfRange, "%v", err)
	}
}

func fieldsErr(f api.Fields) *api.CmdError {
	for k, v := range f {
		return api.Errf(api.CodeOutOfRange, "%s: %s", k, strings.Join(v, "; "))
	}
	return nil
}

// radio.set: any profile fields in one call, all checked first; without
// "profile", the active profile. Replies with the profile.
func (h handlers) cmdSet(_ context.Context, _ *api.Conn, args json.RawMessage) (any, *api.CmdError) {
	var raw map[string]json.RawMessage
	if cerr := api.DecodeArgs(args, &raw); cerr != nil {
		return nil, cerr
	}
	p, f := parsePatch(raw, true)
	if !f.OK() {
		return nil, fieldsErr(f)
	}
	n := h.r.State().ActiveProfile
	if p.profile != nil {
		n = *p.profile
	}
	if n >= h.r.ProfileCount() {
		return nil, api.Errf(api.CodeOutOfRange, "profile: from 0 to %d", h.r.ProfileCount()-1)
	}
	if err := h.apply(n, p); err != nil {
		return nil, cmdErr(err)
	}
	prof, _ := h.r.Profile(n)
	return profileOf(prof), nil
}

func (h handlers) cmdSelect(_ context.Context, _ *api.Conn, args json.RawMessage) (any, *api.CmdError) {
	var a struct {
		Profile *int `json:"profile"`
	}
	if cerr := api.DecodeArgs(args, &a); cerr != nil {
		return nil, cerr
	}
	if a.Profile == nil {
		return nil, api.Errf(api.CodeMissingArg, "profile")
	}
	if *a.Profile < 0 || *a.Profile >= h.r.ProfileCount() {
		return nil, api.Errf(api.CodeOutOfRange, "profile: from 0 to %d", h.r.ProfileCount()-1)
	}
	if err := h.r.SelectProfile(*a.Profile); err != nil {
		return nil, cmdErr(err)
	}
	return stateOf(h.r.State()), nil
}

func (h handlers) cmdResetProtection(_ context.Context, _ *api.Conn, args json.RawMessage) (any, *api.CmdError) {
	var a struct{}
	if cerr := api.DecodeArgs(args, &a); cerr != nil {
		return nil, cerr
	}
	h.r.ResetProtection()
	return nil, nil
}
