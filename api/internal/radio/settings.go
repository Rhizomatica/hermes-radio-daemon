package radio

import (
	"encoding/json"
	"fmt"
	"math"
	"net/http"
	"strings"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
)

// Settings is RadioSettings: what radiod keeps once for the station.
type Settings struct {
	DefaultProfile          int     `json:"default_profile"`
	DefaultProfileFallbackS *int    `json:"default_profile_fallback_s"`
	StepHz                  uint32  `json:"step_hz"`
	SWRLimit                float64 `json:"swr_limit"`
	BfoHz                   *uint32 `json:"bfo_hz"`
	TxTimeoutS              *int    `json:"tx_timeout_s"` // radiod has none yet: always null
}

func settingsOf(s rad.Settings) Settings {
	out := Settings{DefaultProfile: s.DefaultProfile, StepHz: s.StepHz, SWRLimit: float64(s.SWRLimitX10) / 10}
	if s.DefaultProfileFallback >= 0 {
		n := s.DefaultProfileFallback
		out.DefaultProfileFallbackS = &n
	}
	if s.HasBfo {
		b := s.BfoHz
		out.BfoHz = &b
	}
	return out
}

func (h handlers) getSettings(w http.ResponseWriter, r *http.Request) {
	api.JSON(w, http.StatusOK, settingsOf(h.r.Settings()))
}

// updateSettings checks every field (RadioSettingsPatch) before it applies
// any; then answers with the settings as they are.
func (h handlers) updateSettings(w http.ResponseWriter, r *http.Request) {
	var raw map[string]json.RawMessage
	if !api.Decode(w, r, &raw) {
		return
	}
	cur := h.r.Settings()
	f := api.Fields{}
	var steps []func() error
	intIn := func(key string, lo, hi int64) (int64, bool) {
		var v int64
		if err := json.Unmarshal(raw[key], &v); err != nil {
			f.Add(key, "an integer")
			return 0, false
		}
		if v < lo || v > hi {
			f.Add(key, fmt.Sprintf("from %d to %d", lo, hi))
			return 0, false
		}
		return v, true
	}
	for key := range raw {
		switch key {
		case "default_profile":
			if v, ok := intIn(key, 0, int64(h.r.ProfileCount()-1)); ok {
				steps = append(steps, func() error { return h.r.SetDefaultProfile(int(v)) })
			}
		case "default_profile_fallback_s":
			if strings.TrimSpace(string(raw[key])) == "null" {
				steps = append(steps, func() error { return h.r.SetDefaultProfileFallback(-1) })
			} else if v, ok := intIn(key, 1, math.MaxInt32); ok {
				steps = append(steps, func() error { return h.r.SetDefaultProfileFallback(int(v)) })
			}
		case "step_hz":
			if v, ok := intIn(key, 1, math.MaxUint32); ok {
				steps = append(steps, func() error { return h.r.SetStep(uint32(v)) })
			}
		case "swr_limit":
			var v float64
			if err := json.Unmarshal(raw[key], &v); err != nil {
				f.Add(key, "a number")
			} else if x10 := math.Round(v * 10); v < 1 || v > 100 || math.Abs(v*10-x10) > 1e-6 {
				f.Add(key, "from 1.0 to 100.0, in steps of 0.1")
			} else {
				steps = append(steps, func() error { return h.r.SetSWRLimit(uint32(x10)) })
			}
		case "bfo_hz":
			if !cur.HasBfo {
				f.Add(key, "this radio has no BFO (the sBitx has)")
			} else if v, ok := intIn(key, 0, math.MaxUint32); ok {
				steps = append(steps, func() error { return h.r.SetBfo(uint32(v)) })
			}
		case "tx_timeout_s":
			f.Add(key, "this radio has no transmit timeout yet")
		default:
			f.Add(key, "unknown field")
		}
	}
	if !f.OK() {
		api.Invalid(w, r, f)
		return
	}
	for _, s := range steps {
		if err := s(); err != nil {
			h.problemFor(w, r, err)
			return
		}
	}
	api.JSON(w, http.StatusOK, settingsOf(h.r.Settings()))
}
