package radio

import (
	"encoding/json"
	"strings"
	"sync"
)

// Stub is an in-memory Radio for tests and Go-only development: two
// profiles, the setters' range checks of the real one, and whatever frames
// the API layer sent (Sent).
type Stub struct {
	mu       sync.Mutex
	state    State
	profiles []Profile
	settings Settings
	digi     DigiSettings
	log      []json.RawMessage
	modem    json.RawMessage
	Sent     map[int64][][]byte // frames by connection; 0 = broadcast
}

// NewStub returns a Stub tuned to 7.1 MHz USB.
func NewStub() *Stub {
	s := &Stub{
		state: State{FrequencyHz: 7100000, Mode: "USB", Power: 10, Backend: "hamlib"},
		profiles: []Profile{
			{Profile: 0, FrequencyHz: 7100000, Mode: "USB", Power: 10, Volume: 50},
			{Profile: 1, FrequencyHz: 14200000, Mode: "USB", Power: 100, Volume: 50},
		},
		settings: Settings{DefaultProfile: 0, DefaultProfileFallback: -1, StepHz: 100, SWRLimitX10: 25},
		digi: DigiSettings{CWWPM: 20, CWPitchHz: 700, RTTYBaud: 45, RTTYShiftHz: 170, RTTYMarkHz: 2125,
			DstarMycall: "N0CALL", VoiceCodec: "RADEV2"},
		Sent: map[int64][][]byte{},
	}
	return s
}

// SetTX puts the stub in or out of transmit (for "busy" tests).
func (s *Stub) SetTX(tx bool) { s.mu.Lock(); s.state.TX = tx; s.mu.Unlock() }

// AddDigi appends a digi log entry (one JSON object).
func (s *Stub) AddDigi(entry string) {
	s.mu.Lock()
	s.log = append(s.log, json.RawMessage(entry))
	s.mu.Unlock()
}

// SetModem sets mercury's last status (nil: mercury not running).
func (s *Stub) SetModem(status string) {
	s.mu.Lock()
	if status == "" {
		s.modem = nil
	} else {
		s.modem = json.RawMessage(status)
	}
	s.mu.Unlock()
}

func (s *Stub) State() State {
	s.mu.Lock()
	defer s.mu.Unlock()
	st := s.state
	p := s.profiles[st.ActiveProfile]
	st.FrequencyHz, st.Mode, st.Power, st.DigitalVoice = p.FrequencyHz, p.Mode, p.Power, p.DigitalVoice
	return st
}

func (s *Stub) ProfileCount() int { s.mu.Lock(); defer s.mu.Unlock(); return len(s.profiles) }

func (s *Stub) Profile(n int) (Profile, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if n < 0 || n >= len(s.profiles) {
		return Profile{}, ErrNoProfile
	}
	return s.profiles[n], nil
}

func (s *Stub) with(n int, f func(p *Profile) error) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if n < 0 || n >= len(s.profiles) {
		return ErrNoProfile
	}
	return f(&s.profiles[n])
}

func (s *Stub) all(f func(p *Profile)) {
	s.mu.Lock()
	for i := range s.profiles {
		f(&s.profiles[i])
	}
	s.mu.Unlock()
}

func (s *Stub) SelectProfile(n int) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if n < 0 || n >= len(s.profiles) {
		return ErrNoProfile
	}
	if s.state.TX {
		return ErrBusy
	}
	s.state.ActiveProfile = n
	return nil
}

var stubModes = "LSB USB CW FM AM DRM FT8 RTTY DSTAR"

func (s *Stub) SetFrequency(n int, hz uint32) error {
	return s.with(n, func(p *Profile) error { p.FrequencyHz = hz; return nil })
}

func (s *Stub) SetMode(n int, mode string) error {
	return s.with(n, func(p *Profile) error {
		m := strings.ToUpper(mode)
		if m == "" || !strings.Contains(" "+stubModes+" ", " "+m+" ") {
			return ErrInvalid
		}
		p.Mode = m
		return nil
	})
}

func (s *Stub) SetPower(n int, v int) error {
	return s.with(n, func(p *Profile) error {
		if v < 0 || v > 100 {
			return ErrInvalid
		}
		p.Power = v
		return nil
	})
}

func (s *Stub) SetVolume(n int, v int) error {
	return s.with(n, func(p *Profile) error {
		if v < 0 || v > 100 {
			return ErrInvalid
		}
		p.Volume = v
		return nil
	})
}

func (s *Stub) SetFilterWidth(n int, hz uint32) error {
	return s.with(n, func(p *Profile) error { p.FilterWidthHz = hz; return nil })
}

func (s *Stub) SetDigitalVoice(n int, on bool) error {
	if s.State().TX {
		return ErrBusy
	}
	return s.with(n, func(p *Profile) error { p.DigitalVoice = on; return nil })
}

func (s *Stub) ResetProtection() { s.mu.Lock(); s.state.Protection = false; s.mu.Unlock() }

// UseSBitx makes the stub an sBitx, which has a BFO (for tests).
func (s *Stub) UseSBitx() {
	s.mu.Lock()
	s.state.Backend = "sbitx"
	s.settings.HasBfo, s.settings.BfoHz = true, 40035000
	s.mu.Unlock()
}

func (s *Stub) Settings() Settings { s.mu.Lock(); defer s.mu.Unlock(); return s.settings }

func (s *Stub) setting(f func(st *Settings) error) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	return f(&s.settings)
}

func (s *Stub) SetDefaultProfile(n int) error {
	return s.setting(func(st *Settings) error {
		if n < 0 || n >= len(s.profiles) {
			return ErrNoProfile
		}
		st.DefaultProfile = n
		return nil
	})
}

func (s *Stub) SetDefaultProfileFallback(sec int) error {
	return s.setting(func(st *Settings) error {
		if sec < 1 && sec != -1 {
			return ErrInvalid
		}
		st.DefaultProfileFallback = sec
		return nil
	})
}

func (s *Stub) SetStep(hz uint32) error {
	return s.setting(func(st *Settings) error {
		if hz < 1 {
			return ErrInvalid
		}
		st.StepHz = hz
		return nil
	})
}

func (s *Stub) SetSWRLimit(v uint32) error {
	return s.setting(func(st *Settings) error {
		if v < 10 {
			return ErrInvalid
		}
		st.SWRLimitX10 = v
		return nil
	})
}

func (s *Stub) SetBfo(hz uint32) error {
	return s.setting(func(st *Settings) error {
		if !st.HasBfo {
			return ErrNotSupported
		}
		st.BfoHz = hz
		return nil
	})
}

// TripProtection sets the high-SWR protection (for tests).
func (s *Stub) TripProtection() { s.mu.Lock(); s.state.Protection = true; s.mu.Unlock() }

func (s *Stub) DigiSettings() DigiSettings { s.mu.Lock(); defer s.mu.Unlock(); return s.digi }

// The ranges radiod's cfg_digi_param_valid enforces.
func (s *Stub) SetDigi(key string, v int) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	d := &s.digi
	in := func(lo, hi int) bool { return v >= lo && v <= hi }
	switch key {
	case "cw_wpm":
		if !in(5, 60) {
			return ErrInvalid
		}
		d.CWWPM = v
	case "cw_pitch":
		if !in(300, 1200) {
			return ErrInvalid
		}
		d.CWPitchHz = v
	case "rtty_baud":
		if v != 45 && v != 50 && v != 75 {
			return ErrInvalid
		}
		d.RTTYBaud = v
	case "rtty_shift":
		if v != 170 && v != 425 && v != 850 {
			return ErrInvalid
		}
		d.RTTYShiftHz = v
	case "rtty_mark":
		if !in(300, 3000) {
			return ErrInvalid
		}
		d.RTTYMarkHz = v
	case "dstar_encrypt":
		d.DstarEncrypt = v != 0
	case "dstar_denoise":
		d.DstarDenoise = v != 0
	case "dstar_verbose":
		d.DstarVerbose = v != 0
	default:
		return ErrInvalid
	}
	return nil
}

func (s *Stub) SetVoiceCodec(name string) error {
	switch strings.ToUpper(name) {
	case "RADEV2":
	case "DSTAR":
		if s.State().Backend != "sbitx" {
			return ErrNotSupported
		}
	default:
		return ErrInvalid
	}
	s.mu.Lock()
	s.digi.VoiceCodec = strings.ToUpper(name)
	s.mu.Unlock()
	return nil
}

func (s *Stub) DigiLog(after uint64, max int) []json.RawMessage {
	s.mu.Lock()
	defer s.mu.Unlock()
	var out []json.RawMessage
	for _, e := range s.log {
		var head struct {
			ID uint64 `json:"id"`
		}
		if json.Unmarshal(e, &head) == nil && head.ID > after {
			out = append(out, e)
			if len(out) == max {
				break
			}
		}
	}
	return out
}

func (s *Stub) ModemStatus() (json.RawMessage, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.modem, s.modem != nil
}

func (s *Stub) WSSend(conn int64, f []byte) error {
	s.mu.Lock()
	s.Sent[conn] = append(s.Sent[conn], append([]byte(nil), f...))
	s.mu.Unlock()
	return nil
}

func (s *Stub) WSBroadcast(f []byte) error { return s.WSSend(0, f) }

// Frames returns a copy of what was sent to conn (0: broadcasts).
func (s *Stub) Frames(conn int64) [][]byte {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([][]byte(nil), s.Sent[conn]...)
}

func (s *Stub) Version() string { return "stub" }
