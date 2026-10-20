// Package radio is the Go side of the radiod C API (api/radiod_api.h):
// what API handlers may read and change in the daemon.
//
// Handlers take a Radio. In radiod (build tag `radiod`) Default() is the
// C API through the cgo binding; otherwise it is a Stub, so the API code builds and is
// tested with no C toolchain and no radio.
package radio

import (
	"encoding/json"
	"errors"
)

// Errors of the setters, matching radiod_api.h.
var (
	ErrNoProfile    = errors.New("no such profile")
	ErrInvalid      = errors.New("a value the radio does not take")
	ErrBusy         = errors.New("refused while transmitting")
	ErrNotSupported = errors.New("not on this radio or build")
	ErrIO           = errors.New("the rig refused or did not answer")
)

// ControlKind is a control's kind in radiod's control vocabulary
// (radio/radio_controls.h), whichever backend drives the radio.
type ControlKind int

const (
	Level ControlKind = 0 // a value: AF, RF, RFPOWER, PREAMP, ...
	Func  ControlKind = 1 // on/off: NB, COMP, VOX, ...
	Parm  ControlKind = 2 // rig-wide: BACKLIGHT, ANN, ...
)

// Control is one of the rig's controls, as its capabilities describe it.
type Control struct {
	Name           string // as radio/radio_controls.h names it
	Kind           ControlKind
	Float          bool // else an integer (dB, an index, Hz)
	CanGet, CanSet bool
	Min, Max, Step float64 // Max <= Min: the rig gives no range
}

// State is the radio's live state.
type State struct {
	ActiveProfile int
	TX            bool
	FrequencyHz   uint32
	Mode          string // LSB, USB, CW, FM, AM, DRM, FT8, RTTY, DSTAR
	Power         int    // percent
	FwdW, RefW    float64
	SWR           float64
	SMeterValid   bool
	SMeterDB      float64 // dB relative to S9
	Protection    bool
	DigitalVoice  bool
	Backend       string // sbitx, hamlib
}

// Profile is one saved profile.
type Profile struct {
	Profile       int
	FrequencyHz   uint32
	Mode          string
	Power         int
	Volume        int
	FilterWidthHz uint32 // 0: the rig's default
	DigitalVoice  bool
}

// Settings are what radiod keeps once for the station, not per profile.
type Settings struct {
	DefaultProfile         int
	DefaultProfileFallback int // seconds before returning to DefaultProfile; -1: never
	StepHz                 uint32
	SWRLimitX10            uint32 // SWR x 10 that trips high-SWR protection
	HasBfo                 bool   // the sBitx; false on Hamlib radios
	BfoHz                  uint32
}

// DigiSettings are the CW, RTTY and D-STAR settings.
type DigiSettings struct {
	CWWPM, CWPitchHz                         int
	RTTYBaud, RTTYShiftHz, RTTYMarkHz        int
	DstarMycall                              string
	DstarEncrypt, DstarDenoise, DstarVerbose bool
	VoiceCodec                               string // RADEV2, DSTAR
}

// LogLevel is radiod's logger's level: hermes_log's, as cgo/hermeslog.Level
// (this package's stub build has no cgo, so it keeps its own type).
type LogLevel int

const (
	LogDebug  LogLevel = 0
	LogTiming LogLevel = 1
	LogInfo   LogLevel = 2
	LogWarn   LogLevel = 3
	LogError  LogLevel = 4
)

// Radio is what the API layer may do with the daemon. Every method is safe
// from any goroutine.
type Radio interface {
	State() State
	ProfileCount() int
	Profile(n int) (Profile, error)

	SelectProfile(n int) error
	SetFrequency(profile int, hz uint32) error
	SetMode(profile int, mode string) error
	SetPower(profile int, percent int) error
	SetVolume(profile int, percent int) error
	SetFilterWidth(profile int, hz uint32) error
	SetDigitalVoice(profile int, on bool) error
	ResetProtection()

	Settings() Settings
	SetDefaultProfile(n int) error
	SetDefaultProfileFallback(seconds int) error // -1: never
	SetStep(hz uint32) error
	SetSWRLimit(swrX10 uint32) error
	SetBfo(hz uint32) error // ErrNotSupported on Hamlib radios

	// Controls lists the rig's controls (capabilities, no CAT traffic).
	Controls() []Control
	// GetControl reads one control from the rig (a CAT read): its kind and
	// value (0 or 1 for a Func). ErrNotSupported, ErrIO.
	GetControl(name string) (ControlKind, float64, error)
	// SetControl sets one control (a Func: value != 0 is on). ErrInvalid
	// outside its range, ErrNotSupported, ErrIO.
	SetControl(name string, value float64) error

	DigiSettings() DigiSettings
	// SetDigi takes radiod's keys: cw_wpm, cw_pitch, rtty_baud, rtty_mark,
	// rtty_shift, dstar_verbose, dstar_denoise, dstar_encrypt.
	SetDigi(key string, value int) error
	SetVoiceCodec(name string) error
	// DigiLog returns up to max entries after id `after`, oldest first,
	// each one JSON object in digi_log's line format.
	DigiLog(after uint64, max int) []json.RawMessage

	// ModemStatus is mercury's last status JSON; false without mercury.
	ModemStatus() (json.RawMessage, bool)

	// WSSend queues a hermes.v1 text frame for one connection;
	// WSBroadcast for every hermes.v1 connection.
	WSSend(conn int64, frame []byte) error
	WSBroadcast(frame []byte) error

	// Log writes one line to radiod's logger: its format, file and levels.
	Log(level LogLevel, component, message string)

	Version() string
}

func codeErr(rc int) error {
	switch rc {
	case 0:
		return nil
	case -1:
		return ErrNoProfile
	case -2:
		return ErrInvalid
	case -3:
		return ErrBusy
	case -5:
		return ErrIO
	default:
		return ErrNotSupported
	}
}
