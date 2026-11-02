package radio

import (
	"encoding/json/jsontext"
	"encoding/json/v2"
	"fmt"
	"strings"
	"sync"
	"time"
)

// Stub is an in-memory Radio for tests and Go-only development: two
// profiles, the setters' range checks of the real one, and whatever frames
// the API layer sent (Sent).
type Stub struct {
	mu       sync.Mutex
	state    State
	profiles []Profile
	settings Settings
	controls []Control
	values   map[string]float64
	rigDown  bool
	reads    int
	digi     DigiSettings
	log      []jsontext.Value
	modem    jsontext.Value
	gps      Gps
	listen   string // mercury's listen mode
	busy     string // "arq", "broadcast" or ""
	link     bool   // an ARQ session up
	chat     []ChatLine
	gainDB   float64
	wfall    bool
	logs     []string
	Sent     map[int64][][]byte // frames by connection; 0 = broadcast
	closed   map[int64]WSCloseFrame
	pttOwner int64 // the hermes.v1 connection holding PTT, 0 for none
	digiSent []string
	recRX    bool
	recTX    bool
	spectrum map[[2]int64]bool
	rtc      map[int64]RTCSession
	// RTCMax is how many WebRTC sessions the stub takes (radiod's
	// main:webrtc_max_sessions); 0 for none, as radiod without WebRTC.
	RTCMax int
}

// RTCSession is what a stub WebRTC session was negotiated with.
type RTCSession struct {
	SDP    string
	RX, TX bool
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
		controls: []Control{
			{Name: "AF", Kind: Level, Float: true, CanGet: true, CanSet: true, Min: 0, Max: 1, Step: 0.01},
			{Name: "RF", Kind: Level, Float: true, CanGet: true, CanSet: true, Min: 0, Max: 1, Step: 0.01},
			{Name: "PREAMP", Kind: Level, CanGet: true, CanSet: true, Min: 0, Max: 20, Step: 10},
			{Name: "STRENGTH", Kind: Level, CanGet: true, Min: -54, Max: 60},
			{Name: "NB", Kind: Func, CanGet: true, CanSet: true},
			{Name: "TUNER", Kind: Func, CanSet: true},
			{Name: "BACKLIGHT", Kind: Parm, Float: true, CanGet: true, CanSet: true, Min: 0, Max: 1},
		},
		values: map[string]float64{"AF": 0.5, "RF": 1, "PREAMP": 0, "STRENGTH": -20, "NB": 0, "BACKLIGHT": 0.8},
		digi: DigiSettings{CWWPM: 20, CWPitchHz: 700, RTTYBaud: 45, RTTYShiftHz: 170, RTTYMarkHz: 2125,
			DstarMycall: "N0CALL", VoiceCodec: "RADEV2"},
		gps: Gps{Fix: "none", Settings: GpsSettings{SampleIntervalS: 1, FilePeriodS: 3600,
			Report: GpsReport{IntervalS: 900}}},
		listen: "DATAC3",
		Sent:   map[int64][][]byte{},
		closed: map[int64]WSCloseFrame{},
	}
	return s
}

// SetModemBusy says what holds the stub's modem ("arq", "broadcast" or ""),
// and with "arq" that a session is up.
func (s *Stub) SetModemBusy(why string) {
	s.mu.Lock()
	s.busy, s.link = why, why == "arq"
	s.mu.Unlock()
}

// AddChat stores a received chat line, as mercury would.
func (s *Stub) AddChat(plane, peer, text string) {
	s.mu.Lock()
	s.addChat(plane, "rx", peer, text)
	s.mu.Unlock()
}

func (s *Stub) addChat(plane, dir, peer, text string) {
	var seq uint64 = 1
	if n := len(s.chat); n > 0 {
		seq = s.chat[n-1].Seq + 1
	}
	s.chat = append(s.chat, ChatLine{Seq: seq, TS: 1792800000 + int64(seq), Plane: plane, Dir: dir, Peer: peer, Text: text})
}

// ModemSettings reports what the stub's mercury was set to.
func (s *Stub) ModemSettings() (gainDB float64, waterfall bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.gainDB, s.wfall
}

// SetTX puts the stub in or out of transmit (for "busy" tests).
func (s *Stub) SetTX(tx bool) { s.mu.Lock(); s.state.TX = tx; s.mu.Unlock() }

// AddDigi appends a digi log entry (one JSON object).
func (s *Stub) AddDigi(entry string) {
	s.mu.Lock()
	s.log = append(s.log, jsontext.Value(entry))
	s.mu.Unlock()
}

// SetModem sets mercury's last status (nil: mercury not running).
func (s *Stub) SetModem(status string) {
	s.mu.Lock()
	if status == "" {
		s.modem = nil
	} else {
		s.modem = jsontext.Value(status)
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

// SetProtection trips (or clears) the SWR protection, for tests.
func (s *Stub) SetProtection(on bool) { s.mu.Lock(); s.state.Protection = on; s.mu.Unlock() }

// PTT follows radiod's rules (radiod_api_ptt): one owner at a time, refused
// with the protection tripped, and only the owner unkeys.
func (s *Stub) PTT(conn int64, on bool) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !on {
		if s.pttOwner != conn || conn == 0 {
			return ErrNoPTT
		}
		s.pttOwner, s.state.TX = 0, false
		return nil
	}
	if s.state.Protection {
		return ErrProtection
	}
	if s.pttOwner != 0 && s.pttOwner != conn || s.pttOwner == 0 && s.state.TX {
		return ErrBusy
	}
	s.pttOwner, s.state.TX = conn, true
	return nil
}

// ReleasePTT is radiod's close: unkey if conn holds PTT.
func (s *Stub) ReleasePTT(conn int64) {
	s.mu.Lock()
	if s.pttOwner == conn && conn != 0 {
		s.pttOwner, s.state.TX = 0, false
	}
	s.mu.Unlock()
}

func (s *Stub) Controls() []Control {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]Control(nil), s.controls...)
}

func (s *Stub) control(name string) (Control, bool) {
	for _, c := range s.controls {
		if c.Name == name {
			return c, true
		}
	}
	return Control{}, false
}

func (s *Stub) GetControl(name string) (ControlKind, float64, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	c, ok := s.control(name)
	if !ok || !c.CanGet {
		return 0, 0, ErrNotSupported
	}
	if s.rigDown {
		return 0, 0, ErrIO
	}
	s.reads++
	return c.Kind, s.values[name], nil
}

func (s *Stub) SetControl(name string, v float64) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	c, ok := s.control(name)
	switch {
	case !ok || !c.CanSet:
		return ErrNotSupported
	case s.rigDown:
		return ErrIO
	case c.Kind != Func && c.Max > c.Min && (v < c.Min || v > c.Max):
		return ErrInvalid
	}
	if c.Kind == Func && v != 0 {
		v = 1
	}
	s.values[name] = v
	return nil
}

// SetControlValue changes a control as if the rig's front panel had (tests).
func (s *Stub) SetControlValue(name string, v float64) {
	s.mu.Lock()
	s.values[name] = v
	s.mu.Unlock()
}

// RigDown makes the rig stop answering (ErrIO), or answer again (tests).
func (s *Stub) RigDown(down bool) { s.mu.Lock(); s.rigDown = down; s.mu.Unlock() }

// ControlReads counts GetControl's reads (tests: CAT traffic).
func (s *Stub) ControlReads() int { s.mu.Lock(); defer s.mu.Unlock(); return s.reads }

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
	case "rade_encrypt":
		d.RadeEncrypt = v != 0
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

// DigiSend follows radiod's checks (radiod_api_digi_send) and logs the text
// as sent at once (dir tx), as radiod does when it goes out.
func (s *Stub) DigiSend(text string) error {
	st := s.State()
	if st.Mode != "FT8" && st.Mode != "CW" && st.Mode != "RTTY" {
		return ErrNotSupported
	}
	if st.Mode == "FT8" && (len(text) > 13 || strings.Trim(strings.ToUpper(text), "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 +-./?") != "") {
		return ErrInvalid
	}
	s.mu.Lock()
	s.digiSent = append(s.digiSent, text)
	id := uint64(len(s.log) + 1)
	s.mu.Unlock()
	b, _ := json.Marshal(map[string]any{"id": id, "at": time.Now().UTC().Format(time.RFC3339), "mode": st.Mode,
		"dir": "tx", "text": text, "frequency_hz": st.FrequencyHz}, json.Deterministic(true))
	s.AddDigi(string(b))
	return nil
}

// SpectrumSubscribe records the subscription (Spectrum); the stub sends no
// frames.
func (s *Stub) SpectrumSubscribe(conn int64, sp Spectrum, on bool) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.spectrum == nil {
		s.spectrum = map[[2]int64]bool{}
	}
	k := [2]int64{conn, int64(sp)}
	if on && sp == SpectrumModem && s.modem == nil {
		return ErrNotSupported // mercury is not running
	}
	if on {
		s.spectrum[k] = true
	} else {
		delete(s.spectrum, k)
	}
	return nil
}

// Spectrum reports whether conn is subscribed to spectrum sp.
func (s *Stub) Spectrum(conn int64, sp Spectrum) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.spectrum[[2]int64{conn, int64(sp)}]
}

// RTCOffer answers an offer that carries an audio section, as radiod
// does, with every session up to RTCMax; the answer names the session.
func (s *Stub) RTCOffer(conn int64, sdp string, rx, tx bool) (string, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.RTCMax == 0 {
		return "", ErrNotSupported
	}
	if (!rx && !tx) || !strings.Contains(sdp, "m=audio ") {
		return "", ErrInvalid
	}
	if s.rtc == nil {
		s.rtc = map[int64]RTCSession{}
	}
	if _, ok := s.rtc[conn]; !ok && len(s.rtc) >= s.RTCMax {
		return "", ErrBusy
	}
	s.rtc[conn] = RTCSession{SDP: sdp, RX: rx, TX: tx}
	return fmt.Sprintf("v=0\r\no=radiod %d 0 IN IP4 127.0.0.1\r\n", conn), nil
}

// RTCClose ends conn's session.
func (s *Stub) RTCClose(conn int64) {
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.rtc, conn)
}

// RTC reports conn's session, if it has one.
func (s *Stub) RTC(conn int64) (RTCSession, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	r, ok := s.rtc[conn]
	return r, ok
}

// Recording keeps which streams are on, as radiod does.
func (s *Stub) Recording(start bool, stream string) (bool, bool, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if stream != "rx" && stream != "tx" && stream != "both" {
		return s.recRX, s.recTX, ErrInvalid
	}
	if stream != "tx" {
		s.recRX = start
	}
	if stream != "rx" {
		s.recTX = start
	}
	return s.recRX, s.recTX, nil
}

// DigiSent returns what DigiSend queued.
func (s *Stub) DigiSent() []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]string(nil), s.digiSent...)
}

func (s *Stub) DigiLog(after uint64, max int) []jsontext.Value {
	s.mu.Lock()
	defer s.mu.Unlock()
	var out []jsontext.Value
	for _, e := range s.log {
		var head struct {
			ID uint64 `json:"id"`
		}
		if json.Unmarshal(e, &head, FromC) == nil && head.ID > after {
			out = append(out, e)
			if len(out) == max {
				break
			}
		}
	}
	return out
}

func (s *Stub) ModemStatus() (jsontext.Value, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.modem, s.modem != nil
}

// SetGpsFix puts the stub's fix (lat/lon/alt nil: no position; at nil: none).
func (s *Stub) SetGpsFix(fix string, lat, lon, alt *float64, at *string) {
	s.mu.Lock()
	s.gps.Fix = fix
	s.gps.Latitude, s.gps.Longitude, s.gps.AltitudeM, s.gps.At = lat, lon, alt, at
	s.mu.Unlock()
}

func (s *Stub) Gps() Gps {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.gps
}

func (s *Stub) SetGpsSettings(g GpsSettings) error {
	// The ranges api-rest.yaml gives GpsSettings.
	if g.SampleIntervalS < 1 || g.FilePeriodS < 60 || g.Report.IntervalS < 60 {
		return ErrInvalid
	}
	s.mu.Lock()
	s.gps.Settings = g
	s.mu.Unlock()
	return nil
}

// stubListenModes are the listen modes mercury can switch to at run time.
var stubListenModes = map[string]bool{"DATAC1": true, "DATAC3": true, "DATAC4": true, "DATAC15": true,
	"DATAC17": true, "QAM16C2": true, "MFSK": true, "MFSK500": true}

func (s *Stub) running() bool { return s.modem != nil }

func (s *Stub) ListenMode() (string, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.running() {
		return "", ErrNotSupported
	}
	return s.listen, nil
}

func (s *Stub) SetListenMode(name string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	switch {
	case !s.running():
		return ErrNotSupported
	case !stubListenModes[name]:
		return ErrInvalid
	case s.busy != "":
		return ErrBusy
	}
	s.listen = name
	return nil
}

func (s *Stub) ModemBusy() (string, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.running() {
		return "", ErrNotSupported
	}
	return s.busy, nil
}

func (s *Stub) SetModemTxGain(db float64) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.running() {
		return ErrNotSupported
	}
	if db < -20 || db > 20 {
		return ErrInvalid
	}
	s.gainDB = db
	return nil
}

func (s *Stub) SetModemWaterfall(on bool) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.running() {
		return ErrNotSupported
	}
	s.wfall = on
	return nil
}

func (s *Stub) ChatSince(after uint64) ([]ChatLine, uint64, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.running() {
		return nil, after, ErrNotSupported
	}
	var out []ChatLine
	for _, l := range s.chat {
		if l.Seq > after {
			out = append(out, l)
			after = l.Seq
		}
	}
	return out, after, nil
}

// stubBcastMax is the stub's broadcast chat line limit (DATAC3's).
const stubBcastMax = 120

func (s *Stub) ChatSend(plane, text string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	text = strings.TrimRight(text, "\r\n")
	switch {
	case !s.running():
		return ErrNotSupported
	case text == "" || len(text) > 1024:
		return ErrInvalid
	case plane == "arq" && !s.link:
		return ErrNoLink
	case plane == "broadcast" && len(text) > stubBcastMax:
		return ErrTooLong
	case plane != "arq" && plane != "broadcast":
		return ErrInvalid
	}
	p := plane
	if p == "broadcast" {
		p = "bcast"
	}
	s.addChat(p, "tx", "", text)
	return nil
}

func (s *Stub) WSSend(conn int64, f []byte) error {
	s.mu.Lock()
	s.Sent[conn] = append(s.Sent[conn], append([]byte(nil), f...))
	s.mu.Unlock()
	return nil
}

func (s *Stub) WSBroadcast(f []byte) error { return s.WSSend(0, f) }

// WSClose records the close (Closed); the test calls the router's WSClose
// itself, as radiod's C server would.
func (s *Stub) WSClose(conn int64, code int, reason string) error {
	s.mu.Lock()
	s.closed[conn] = WSCloseFrame{Code: code, Reason: reason}
	s.mu.Unlock()
	return nil
}

// WSCloseFrame is a close the API layer asked for.
type WSCloseFrame struct {
	Code   int
	Reason string
}

// Closed returns the close asked for conn, and whether there was one.
func (s *Stub) Closed(conn int64) (WSCloseFrame, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	c, ok := s.closed[conn]
	return c, ok
}

// Frames returns a copy of what was sent to conn (0: broadcasts).
func (s *Stub) Frames(conn int64) [][]byte {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([][]byte(nil), s.Sent[conn]...)
}

func (s *Stub) Version() string { return "stub" }

// CORSOrigins is none: a test sets them on the router (SetCORSOrigins).
func (s *Stub) CORSOrigins() string { return "" }

var logLevelNames = [...]string{"TRC", "DBG", "TMG", "INF", "WRN", "ERR"} // from LogTrace

// Log records the line as "[LVL] [component] message", the part of
// hermes_log's format after the clock (Logs returns them).
func (s *Stub) Log(level LogLevel, component, message string) {
	name := "INF"
	if i := int(level - LogTrace); i >= 0 && i < len(logLevelNames) {
		name = logLevelNames[i]
	}
	s.mu.Lock()
	s.logs = append(s.logs, "["+name+"] ["+component+"] "+message)
	s.mu.Unlock()
}

// Logs returns a copy of the lines logged so far.
func (s *Stub) Logs() []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return append([]string(nil), s.logs...)
}
