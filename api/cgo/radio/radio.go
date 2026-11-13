// Package radio is the Go side of the radiod C API (api/radiod_api.h):
// what API handlers may read and change in the daemon.
//
// Handlers take a Radio. In radiod (build tag `radiod`) Default() is the
// C API through the cgo binding; otherwise it is a Stub, so the API code builds and is
// tested with no C toolchain and no radio.
package radio

import (
	"encoding/json/jsontext"
	"errors"
	"math"
)

// Errors of the setters, matching radiod_api.h.
var (
	ErrNoProfile    = errors.New("no such profile")
	ErrInvalid      = errors.New("a value the radio does not take")
	ErrBusy         = errors.New("refused while transmitting")
	ErrNotSupported = errors.New("not on this radio or build")
	ErrIO           = errors.New("the rig refused or did not answer")
	ErrNoLink       = errors.New("the modem has no ARQ session up")
	ErrTooLong      = errors.New("too long for one broadcast frame")
	ErrProtection   = errors.New("the SWR protection has tripped")
	ErrNoPTT        = errors.New("this client does not hold PTT")
	ErrDown         = errors.New("the broadcast plane is not running")
	ErrNotFound     = errors.New("no such file")
	ErrNoPosition   = errors.New("no callsign or no position to send")
	ErrRange        = errors.New("a value out of the range its format carries")
)

// FromC is how Go reads JSON that radiod or mercury wrote. Its text can
// come from the air with invalid UTF-8, which encoding/json/v2 refuses by
// default; it reads as U+FFFD, as encoding/json read it.
var FromC = jsontext.AllowInvalidUTF8(true)

// Spectrum is one of the spectrums C sends as hermes.v1 binary frames
// (radiod_api_spectrum).
type Spectrum int

const (
	SpectrumRX    Spectrum = iota // the radio's RX spectrum, kind 3
	SpectrumTX                    // the radio's TX spectrum, kind 4
	SpectrumModem                 // mercury's, kind 3
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
	ActiveProfile  int
	TX             bool
	FrequencyHz    uint32
	Mode           string // LSB, USB, CW, FM, AM, DRM, FT8, RTTY, DSTAR
	Power          int    // percent
	FwdW, RefW     float64
	SWR            float64
	SMeterValid    bool
	SMeterDB       float64 // dB relative to S9
	Protection     bool
	DigitalVoice   bool
	NoiseReduction bool   // the active profile's
	Backend        string // sbitx, hamlib
}

// Profile is one saved profile.
type Profile struct {
	Profile        int
	FrequencyHz    uint32
	Mode           string
	Power          int
	Volume         int
	FilterWidthHz  uint32 // 0: the rig's default
	DigitalVoice   bool
	NoiseReduction bool // on the received audio people listen to
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
	RadeEncrypt                              bool
	VoiceCodec                               string // RADEV2, DSTAR
}

// Gps is the station's own position and fix (Gps in api-rest.yaml). At is an
// ISO 8601 UTC timestamp, nil while there is no fix.
type Gps struct {
	Fix       string // none, 2d, 3d
	Latitude  *float64
	Longitude *float64
	AltitudeM *float64
	At        *string
	Settings  GpsSettings
}

// GpsPoint is a point of a station track (radiod_api_gps_point), as the
// track's files hold it: its time in unix milliseconds, its position in
// 1e-7 degrees, its altitude and horizontal accuracy in decimetres (GpsNoAlt,
// GpsNoAcc when unknown), its fix (GpsFix*) and a beacon's emergency code
// (0 none).
type GpsPoint struct {
	AtMs           int64
	LatE7, LonE7   int32
	AltDm          int32
	AccuracyDm     uint16
	Fix, Emergency uint8
}

// A GpsPoint's unknowns and fixes.
const (
	GpsNoAlt     = math.MinInt32
	GpsNoAcc     = math.MaxUint16
	GpsFixNone   = 0 // the last known position
	GpsFix2D     = 2
	GpsFix3D     = 3
	GpsFixManual = 4 // set by hand: a fixed station
)

// GpsSettings are the station's GPS logging settings (GpsSettings in
// api-rest.yaml). Email is nil when none is set. Sending the position over
// the air is the beacon's (Beacon).
type GpsSettings struct {
	Logging         bool
	SampleIntervalS int
	FilePeriodS     int
	Email           *string
}

// BroadcastState is the state of the file on the air (radiod_api.h
// RADIOD_API_BCAST_*).
type BroadcastState int

const (
	BroadcastIdle BroadcastState = iota
	BroadcastSending
	BroadcastDone
	BroadcastStopped
	BroadcastError
)

// Broadcast is radiod's broadcast plane, as Mercury's client: whether it
// runs, Mercury's listen mode, the spool directories (files dropped in TxDir
// go on the air, files received appear in RxDir) and the file on the air.
type Broadcast struct {
	Running, Connected bool
	Mode               int
	ModeName           string
	TxDir, RxDir       string
	State              BroadcastState
	Path               string // the file on the air, in TxDir; "" when none
	Name               string // what receivers file it under
	Bytes              uint64
	Pass, Passes       int // Passes 0: until the file is removed
	FramesPerPass      int64
	FramesLimit        int64 // -1: none
	FramesSent         uint64
}

// TransportSettings are the store-and-forward layer's settings
// (api/internal/transport.Settings): Prefer "uucp" or "nncp", uucico's
// options over the radio, and the systems it reaches over TCP.
type TransportSettings struct {
	Prefer           string
	UucicoAirOptions []string
	UucicoTCPSystems []string
}

// Beacon is the station's position-beacon settings (docs/BEACON-FORMAT.md).
// StationLatitude/StationLongitude are nil without a fixed position.
type Beacon struct {
	Enabled            bool // the regular beacons (main:beacon_enable)
	IntervalS          int  // between them (main:beacon_interval_s)
	Callsign           string
	StationLatitude    *float64
	StationLongitude   *float64
	EmergencyIntervalS int
	Emergency          int  // the code set, 0 none
	Running            bool // beacons go out (the broadcast plane runs)
	// SosRecipients are the stations an SOS also goes to over UUCP or
	// NNCP (main:sos_recipients).
	SosRecipients []string
}

// BeaconStation is a station heard by its beacons: its last report.
type BeaconStation struct {
	Callsign            string
	Latitude, Longitude float64
	Emergency, Fix      int // Fix: 0 none, 1 2-D, 2 3-D, 3 set by hand
	AltitudeM           *int
	HeardAt             string // ISO 8601 UTC
}

// ChatLine is one of mercury's chat lines as it stores them (its
// message_store), sent or received on either plane.
type ChatLine struct {
	Seq   uint64 `json:"seq"`
	TS    int64  `json:"ts"` // Unix seconds
	MS    int    `json:"ms"`
	Plane string `json:"plane"` // "arq" or "bcast"
	Dir   string `json:"dir"`   // "rx" or "tx"
	Peer  string `json:"peer"`
	Text  string `json:"text"`
}

// LogLevel is radiod's logger's level: hermes_log's, as cgo/hermeslog.Level
// (this package's stub build has no cgo, so it keeps its own type).
type LogLevel int

const (
	LogTrace  LogLevel = -1
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
	SetNoiseReduction(profile int, on bool) error
	ResetProtection()
	// PTT keys (on) or unkeys the transmitter for hermes.v1 connection conn,
	// which then owns it; radiod unkeys when that connection closes. On:
	// ErrProtection with the SWR protection tripped, ErrBusy while another
	// client holds PTT. Off: ErrNoPTT when conn does not hold it.
	PTT(conn int64, on bool) error

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
	// rtty_shift, dstar_verbose, dstar_denoise, dstar_encrypt, rade_encrypt.
	SetDigi(key string, value int) error
	SetVoiceCodec(name string) error
	// DigiLog returns up to max entries after id `after`, oldest first,
	// each one JSON object in digi_log's line format.
	DigiLog(after uint64, max int) []jsontext.Value
	// DigiLogFiles returns the log's files on disk, oldest first, each
	// line one entry in the same format: the whole log, where DigiLog has
	// only the in-memory ring's last entries. A file may not exist yet.
	DigiLogFiles() []string
	// GpsTrack returns up to max points of a station's track (station "":
	// this one's; else a callsign) at times sinceMs..untilMs (unix
	// milliseconds, both included), oldest first: ErrInvalid when station
	// is no callsign. A track's times only go forward, so the next page
	// starts 1 ms after the last point's.
	GpsTrack(station string, sinceMs, untilMs int64, max int) ([]GpsPoint, error)
	// GpsTrackCount returns how many points a track has at times
	// sinceMs..untilMs, without reading them: ErrInvalid as GpsTrack.
	GpsTrackCount(station string, sinceMs, untilMs int64) (int, error)
	// GpsTrackCounts is GpsTrackCount for many ranges at once (one pass
	// over the track's files), a count for each range, in order.
	GpsTrackCounts(station string, sinceMs, untilMs []int64) ([]int, error)
	// GpsTrackFiles returns this station's track files (gps/gps_track.h,
	// /gps/files), full paths, oldest first, the newest 65536 of them (none
	// before the directory exists); each one's name, gps-YYYYMMDDTHHMMSSZ.trk,
	// is the start of the period it covers. An error when the directory
	// cannot be read.
	GpsTrackFiles() ([]string, error)
	// GpsTrackDelete deletes one by name (no path): ErrInvalid when that is
	// no track file's name, ErrNotFound when there is no such file, ErrIO
	// when it cannot be removed.
	GpsTrackDelete(name string) error
	// DigiSend queues text for the active profile's digital text mode:
	// ErrNotSupported when that is not FT8, CW or RTTY, ErrInvalid for FT8
	// text the modulator cannot carry, ErrBusy with the queue full. The
	// digi log reports it (dir tx) when it goes out.
	DigiSend(text string) error
	// Recording starts (start) or stops recording stream "rx", "tx" or
	// "both" to WAV files on the station, and says which recordings are on
	// after it: ErrInvalid for another stream, ErrIO when a file could not
	// be opened.
	Recording(start bool, stream string) (rx, tx bool, err error)
	// SpectrumSubscribe subscribes (on) or unsubscribes hermes.v1
	// connection conn to spectrum s, whose binary frames C then sends it:
	// ErrNotSupported when the radio has no such spectrum or mercury is not
	// running, ErrBusy when every slot is taken.
	SpectrumSubscribe(conn int64, s Spectrum, on bool) error
	// RTCOffer starts, or renegotiates, hermes.v1 connection conn's WebRTC
	// audio from the browser's SDP offer (rx: the station's receive audio;
	// tx: the browser's microphone, transmitted only while conn holds PTT)
	// and returns the station's answer. ErrInvalid for an offer with no Opus
	// audio section, ErrBusy with every session taken, ErrIO when
	// negotiation fails, ErrNotSupported without WebRTC. The session ends
	// with the connection.
	RTCOffer(conn int64, sdp string, rx, tx bool) (string, error)
	// RTCClose ends conn's WebRTC session, if it has one.
	RTCClose(conn int64)

	// Gps is the station's own position and fix, and its GPS settings.
	Gps() Gps
	// GpsClockMs is now, unix milliseconds, on the clock this station's
	// track points are stamped with (gps_clock in gps/gps.h): the system
	// clock corrected by the last GPS fix's time.
	GpsClockMs() int64
	// SetGpsSettings applies the whole GPS settings (already validated by
	// the handler). ErrInvalid outside the spec's ranges.
	SetGpsSettings(GpsSettings) error

	// Broadcast is the broadcast plane's state; ErrNotSupported in a build
	// without Mercury. BroadcastFramesPerPass is how many frames one pass
	// of len bytes sent under name takes in the listen mode (0: the mode
	// cannot carry files).
	Broadcast() (Broadcast, error)
	BroadcastFramesPerPass(name string, len uint64) int64
	// BroadcastFileInfo is what the spool makes of a file name in TxDir:
	// the name receivers file it under, and its frame budget (-1: none).
	BroadcastFileInfo(file string) (sendName string, framesLimit int64)

	// Beacon is the beacon settings; SetBeacon saves and applies the
	// callsign, fixed position, emergency interval and SOS recipients
	// (ErrInvalid out of range). SetBeaconEmergency sets the code (1..30)
	// or clears it (0), with or without the broadcast plane (without it no
	// beacon goes on the air); BeaconSendNow sends one, ErrDown while the
	// plane is not running. BeaconFrame is the compact beacon the station
	// would send now with code (0..31), for the SOS over UUCP or NNCP:
	// ErrNoPosition without a callsign or a position, ErrRange for a
	// position the beacon cannot carry. BeaconStations is the stations
	// heard, newest first.
	Beacon() Beacon
	SetBeacon(Beacon) error
	SetBeaconEmergency(code int) error
	BeaconSendNow() error
	BeaconFrame(code int) ([]byte, error)
	BeaconStations() []BeaconStation

	// Transport is the store-and-forward layer's settings (core.ini).
	Transport() TransportSettings

	// ModemStatus is mercury's last status JSON; false without mercury.
	ModemStatus() (jsontext.Value, bool)

	// Mercury's listen mode, its broadcast plane and chat (its host API).
	// ErrNotSupported while mercury is not running.
	//
	// ListenMode is the mode's name ("DATAC3"). SetListenMode sets and
	// saves it: ErrInvalid for a mode mercury cannot switch to at run time,
	// ErrBusy while an ARQ session or a broadcast holds the modem (ModemBusy
	// says which: "arq", "broadcast", or "").
	ListenMode() (string, error)
	SetListenMode(name string) error
	ModemBusy() (string, error)
	// SetModemTxGain (dB, -20 to 20) and SetModemWaterfall, as mercury's UI
	// commands; mercury saves them.
	SetModemTxGain(db float64) error
	SetModemWaterfall(on bool) error
	// ChatSince returns the chat lines after seq after (0: all mercury
	// holds), oldest first, and the seq of the last.
	ChatSince(after uint64) ([]ChatLine, uint64, error)
	// ChatSend sends one line on plane "arq" (ErrNoLink with no session
	// up, ErrBusy while its queue is full) or "broadcast" (one frame in the
	// listen mode: ErrTooLong when it does not fit).
	ChatSend(plane, text string) error

	// WSSend queues a hermes.v1 text frame for one connection;
	// WSBroadcast for every hermes.v1 connection.
	WSSend(conn int64, frame []byte) error
	WSBroadcast(frame []byte) error
	// WSClose closes one hermes.v1 connection with a WebSocket close code
	// and reason, after the frames already queued for it; the C server
	// then calls HermesWSClose as for any other close.
	WSClose(conn int64, code int, reason string) error

	// Log writes one line to radiod's logger: its format, file and levels.
	Log(level LogLevel, component, message string)

	Version() string
	// CORSOrigins is main:api_cors_origins: the origins whose pages may call
	// /api/v2 cross-origin ("" for none).
	CORSOrigins() string
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
	case -6:
		return ErrNoLink
	case -7:
		return ErrTooLong
	case -8:
		return ErrProtection
	case -9:
		return ErrNoPTT
	case -10:
		return ErrDown
	case -11:
		return ErrRange
	default:
		return ErrNotSupported
	}
}
