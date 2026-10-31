// Package digi serves the digital modes' REST operations (api-rest.yaml,
// tag digi): the log of what the radio decoded and sent, and the CW, RTTY
// and D-STAR settings.
package digi

import (
	"encoding/json"
	"errors"
	"net/http"
	"sort"
	"strings"
	"time"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
)

// Register adds the digital modes' operations.
func Register(rt *api.Router) {
	h := handlers{rt.Radio()}
	rt.Op("getDigiLog", h.getLog)
	rt.Op("getDigiSettings", h.getSettings)
	rt.Op("updateDigiSettings", h.updateSettings)
}

type handlers struct{ r rad.Radio }

// ringMax bounds how much of digi_log's in-memory ring one request reads.
const ringMax = 1000

// entryHead is what the filters need of a digi_log line; the line itself
// already has DigiEntry's fields and goes out as is.
type entryHead struct {
	ID   uint64  `json:"id"`
	At   string  `json:"at"`
	Mode string  `json:"mode"`
	Dir  string  `json:"dir"`
	From *string `json:"from"`
	To   *string `json:"to"`
}

func (h handlers) getLog(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	f := api.Fields{}
	mode, dir, call := strings.ToUpper(q.Get("mode")), q.Get("dir"), strings.ToUpper(q.Get("callsign"))
	if mode != "" && mode != "FT8" && mode != "CW" && mode != "RTTY" && mode != "DSTAR" {
		f.Add("mode", "one of FT8, CW, RTTY, DSTAR")
	}
	if dir != "" && dir != "rx" && dir != "tx" {
		f.Add("dir", "rx or tx")
	}
	if len(call) > 16 {
		f.Add("callsign", "at most 16 characters")
	}
	var since, until time.Time
	for key, dst := range map[string]*time.Time{"since": &since, "until": &until} {
		if s := q.Get(key); s != "" {
			t, err := time.Parse(time.RFC3339, s)
			if err != nil {
				f.Add(key, "an ISO 8601 date-time")
			}
			*dst = t
		}
	}
	if !f.OK() {
		api.Invalid(w, r, f)
		return
	}
	limit, ok := api.Limit(w, r)
	if !ok {
		return
	}
	before, ok := api.DecodeCursor(w, r) // newest first: entries with id < cursor
	if !ok {
		return
	}

	var picked []json.RawMessage
	var ids []uint64
	for _, line := range h.r.DigiLog(0, ringMax) {
		var e entryHead
		if json.Unmarshal(line, &e) != nil {
			continue
		}
		if before != 0 && e.ID >= before {
			continue
		}
		if mode != "" && e.Mode != mode || dir != "" && e.Dir != dir {
			continue
		}
		if call != "" && !(e.From != nil && strings.EqualFold(*e.From, call) ||
			e.To != nil && strings.EqualFold(*e.To, call)) {
			continue
		}
		if !since.IsZero() || !until.IsZero() {
			at, err := time.Parse(time.RFC3339Nano, e.At)
			if err != nil || !since.IsZero() && at.Before(since) || !until.IsZero() && at.After(until) {
				continue
			}
		}
		picked = append(picked, line)
		ids = append(ids, e.ID)
	}
	// newest first
	idx := make([]int, len(picked))
	for i := range idx {
		idx[i] = i
	}
	sort.Slice(idx, func(a, b int) bool { return ids[idx[a]] > ids[idx[b]] })
	page := api.Page[json.RawMessage]{Data: []json.RawMessage{}}
	for _, i := range idx {
		if len(page.Data) == limit {
			page.NextCursor = api.EncodeCursor(ids[idx[len(page.Data)-1]])
			break
		}
		page.Data = append(page.Data, picked[i])
	}
	api.JSON(w, http.StatusOK, page)
}

// Settings is DigiSettings.
type Settings struct {
	CW struct {
		WPM     int `json:"wpm"`
		PitchHz int `json:"pitch_hz"`
	} `json:"cw"`
	RTTY struct {
		Baud    int `json:"baud"`
		ShiftHz int `json:"shift_hz"`
		MarkHz  int `json:"mark_hz"`
	} `json:"rtty"`
	Dstar struct {
		Mycall  string `json:"mycall"`
		Encrypt bool   `json:"encrypt"`
		Denoise bool   `json:"denoise"`
		Verbose bool   `json:"verbose"`
	} `json:"dstar"`
	Rade struct {
		Encrypt bool `json:"encrypt"`
	} `json:"rade"`
	VoiceCodec string `json:"voice_codec"`
}

func settingsOf(d rad.DigiSettings) Settings {
	var s Settings
	s.CW.WPM, s.CW.PitchHz = d.CWWPM, d.CWPitchHz
	s.RTTY.Baud, s.RTTY.ShiftHz, s.RTTY.MarkHz = d.RTTYBaud, d.RTTYShiftHz, d.RTTYMarkHz
	s.Dstar.Mycall, s.Dstar.Encrypt = d.DstarMycall, d.DstarEncrypt
	s.Dstar.Denoise, s.Dstar.Verbose = d.DstarDenoise, d.DstarVerbose
	s.Rade.Encrypt = d.RadeEncrypt
	s.VoiceCodec = d.VoiceCodec
	return s
}

func (h handlers) getSettings(w http.ResponseWriter, r *http.Request) {
	api.JSON(w, http.StatusOK, settingsOf(h.r.DigiSettings()))
}

// settingsPatch: every field optional, as PATCH sends only what changes.
type settingsPatch struct {
	CW *struct {
		WPM     *int `json:"wpm"`
		PitchHz *int `json:"pitch_hz"`
	} `json:"cw"`
	RTTY *struct {
		Baud    *int `json:"baud"`
		ShiftHz *int `json:"shift_hz"`
		MarkHz  *int `json:"mark_hz"`
	} `json:"rtty"`
	Dstar *struct {
		Mycall  *string `json:"mycall"`
		Encrypt *bool   `json:"encrypt"`
		Denoise *bool   `json:"denoise"`
		Verbose *bool   `json:"verbose"`
	} `json:"dstar"`
	Rade *struct {
		Encrypt *bool `json:"encrypt"`
	} `json:"rade"`
	VoiceCodec *string `json:"voice_codec"`
}

type set struct {
	key   string // radiod's digi key, or "" for the codec
	value int
	field string // the API field, for errors
}

func (h handlers) updateSettings(w http.ResponseWriter, r *http.Request) {
	var p settingsPatch
	if !api.Decode(w, r, &p) {
		return
	}
	f := api.Fields{}
	var sets []set
	in := func(field string, v, lo, hi int) bool {
		if v < lo || v > hi {
			f.Add(field, "from "+itoa(lo)+" to "+itoa(hi))
			return false
		}
		return true
	}
	oneOf := func(field string, v int, ok ...int) bool {
		for _, o := range ok {
			if v == o {
				return true
			}
		}
		f.Add(field, "one of "+join(ok))
		return false
	}
	if p.CW != nil {
		if p.CW.WPM != nil && in("cw.wpm", *p.CW.WPM, 5, 60) {
			sets = append(sets, set{"cw_wpm", *p.CW.WPM, "cw.wpm"})
		}
		if p.CW.PitchHz != nil && in("cw.pitch_hz", *p.CW.PitchHz, 300, 1200) {
			sets = append(sets, set{"cw_pitch", *p.CW.PitchHz, "cw.pitch_hz"})
		}
	}
	if p.RTTY != nil {
		if p.RTTY.Baud != nil && oneOf("rtty.baud", *p.RTTY.Baud, 45, 50, 75) {
			sets = append(sets, set{"rtty_baud", *p.RTTY.Baud, "rtty.baud"})
		}
		if p.RTTY.ShiftHz != nil && oneOf("rtty.shift_hz", *p.RTTY.ShiftHz, 170, 425, 850) {
			sets = append(sets, set{"rtty_shift", *p.RTTY.ShiftHz, "rtty.shift_hz"})
		}
		if p.RTTY.MarkHz != nil && in("rtty.mark_hz", *p.RTTY.MarkHz, 300, 3000) {
			sets = append(sets, set{"rtty_mark", *p.RTTY.MarkHz, "rtty.mark_hz"})
		}
	}
	if p.Dstar != nil {
		if p.Dstar.Mycall != nil && *p.Dstar.Mycall != h.r.DigiSettings().DstarMycall {
			f.Add("dstar.mycall", "set in radiod's configuration (main:dstar_mycall)")
		}
		for _, b := range []struct {
			v   *bool
			key string
		}{{p.Dstar.Encrypt, "dstar_encrypt"}, {p.Dstar.Denoise, "dstar_denoise"}, {p.Dstar.Verbose, "dstar_verbose"}} {
			if b.v != nil {
				n := 0
				if *b.v {
					n = 1
				}
				sets = append(sets, set{b.key, n, "dstar." + strings.TrimPrefix(b.key, "dstar_")})
			}
		}
	}
	if p.Rade != nil && p.Rade.Encrypt != nil {
		n := 0
		if *p.Rade.Encrypt {
			n = 1
		}
		sets = append(sets, set{"rade_encrypt", n, "rade.encrypt"})
	}
	if p.VoiceCodec != nil {
		c := strings.ToUpper(*p.VoiceCodec)
		if c != "RADEV2" && c != "DSTAR" {
			f.Add("voice_codec", "RADEV2 or DSTAR")
		}
	}
	if !f.OK() {
		api.Invalid(w, r, f)
		return
	}
	if p.VoiceCodec != nil { // first: the one change that can be refused
		if err := h.r.SetVoiceCodec(strings.ToUpper(*p.VoiceCodec)); err != nil {
			switch {
			case errors.Is(err, rad.ErrBusy):
				api.Problem(w, r, http.StatusConflict, "busy", "refused while transmitting", nil)
			default:
				api.Invalid(w, r, api.Fields{"voice_codec": {"DSTAR needs the sBitx backend"}})
			}
			return
		}
	}
	for _, s := range sets {
		if err := h.r.SetDigi(s.key, s.value); err != nil {
			api.Invalid(w, r, api.Fields{s.field: {err.Error()}})
			return
		}
	}
	api.JSON(w, http.StatusOK, settingsOf(h.r.DigiSettings()))
}

func itoa(n int) string { b, _ := json.Marshal(n); return string(b) }

func join(ns []int) string {
	s := make([]string, len(ns))
	for i, n := range ns {
		s[i] = itoa(n)
	}
	return strings.Join(s, ", ")
}
