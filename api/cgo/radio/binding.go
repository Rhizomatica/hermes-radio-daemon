//go:build radiod

package radio

/*
#cgo CFLAGS: -I${SRCDIR}/../..
// The C API's symbols live in radio_daemon, linked after this archive;
// cgo's own check link must not insist on them.
#cgo LDFLAGS: -Wl,--unresolved-symbols=ignore-all
#include <stdlib.h>
#include "radiod_api.h"
*/
import "C"

import (
	"bytes"
	"encoding/json"
	"strings"
	"time"
	"unsafe"

	"github.com/Rhizomatica/hermes-radio-daemon/api/cgo/hermeslog"
)

// Default is the cgo binding to the radiod C API.
func Default() Radio { return binding{} }

type binding struct{}

func cstr(b []C.char) string { return C.GoString(&b[0]) }

func (binding) State() State {
	var s C.radiod_api_state
	C.radiod_api_state_get(&s)
	return State{
		ActiveProfile: int(s.active_profile), TX: bool(s.tx), FrequencyHz: uint32(s.frequency_hz),
		Mode: cstr(s.mode[:]), Power: int(s.power), FwdW: float64(s.fwd_x10) / 10, RefW: float64(s.ref_x10) / 10,
		SWR: float64(s.swr_x10) / 10, SMeterValid: bool(s.s_meter_valid), SMeterDB: float64(s.s_meter_db),
		Protection: bool(s.protection), DigitalVoice: bool(s.digital_voice),
		Backend: cstr(s.backend[:]),
	}
}

func (binding) ProfileCount() int { return int(C.radiod_api_profile_count()) }

func (binding) Profile(n int) (Profile, error) {
	if n < 0 {
		return Profile{}, ErrNoProfile
	}
	var p C.radiod_api_profile
	if err := codeErr(int(C.radiod_api_profile_get(C.uint32_t(n), &p))); err != nil {
		return Profile{}, err
	}
	return Profile{
		Profile: int(p.profile), FrequencyHz: uint32(p.frequency_hz), Mode: cstr(p.mode[:]),
		Power: int(p.power), Volume: int(p.volume),
		FilterWidthHz: uint32(p.filter_width_hz), DigitalVoice: bool(p.digital_voice),
	}, nil
}

func prof(n int) C.uint32_t {
	if n < 0 {
		return C.uint32_t(^uint32(0)) // never a profile: ENOENT
	}
	return C.uint32_t(n)
}

func (binding) SelectProfile(n int) error { return codeErr(int(C.radiod_api_select_profile(prof(n)))) }

func (binding) DigiSend(text string) error {
	ct := C.CString(text)
	defer C.free(unsafe.Pointer(ct))
	return codeErr(int(C.radiod_api_digi_send(ct)))
}

func (binding) PTT(conn int64, on bool) error {
	return codeErr(int(C.radiod_api_ptt(C.long(conn), C.bool(on))))
}
func (binding) SetFrequency(n int, hz uint32) error {
	return codeErr(int(C.radiod_api_set_frequency(prof(n), C.uint32_t(hz))))
}
func (binding) SetMode(n int, mode string) error {
	cs := C.CString(mode)
	defer C.free(unsafe.Pointer(cs))
	return codeErr(int(C.radiod_api_set_mode(prof(n), cs)))
}
func (binding) SetPower(n int, v int) error {
	return codeErr(int(C.radiod_api_set_power(prof(n), C.int(v))))
}
func (binding) SetVolume(n int, v int) error {
	return codeErr(int(C.radiod_api_set_volume(prof(n), C.int(v))))
}
func (binding) SetFilterWidth(n int, hz uint32) error {
	return codeErr(int(C.radiod_api_set_filter_width(prof(n), C.uint32_t(hz))))
}
func (binding) SetDigitalVoice(n int, on bool) error {
	return codeErr(int(C.radiod_api_set_digital_voice(prof(n), C.bool(on))))
}
func (binding) ResetProtection() { C.radiod_api_reset_protection() }

func (binding) Controls() []Control {
	var cs [192]C.radiod_api_control
	n := int(C.radiod_api_controls(&cs[0], C.int(len(cs))))
	out := make([]Control, 0, n)
	for _, c := range cs[:n] {
		out = append(out, Control{
			Name: cstr(c.name[:]), Kind: ControlKind(c.kind), Float: bool(c.is_float),
			CanGet: bool(c.can_get), CanSet: bool(c.can_set),
			Min: float64(c.min), Max: float64(c.max), Step: float64(c.step),
		})
	}
	return out
}

func (binding) GetControl(name string) (ControlKind, float64, error) {
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	var kind C.int
	var v C.double
	if err := codeErr(int(C.radiod_api_control_get(cs, &kind, &v))); err != nil {
		return 0, 0, err
	}
	return ControlKind(kind), float64(v), nil
}

func (binding) SetControl(name string, value float64) error {
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	return codeErr(int(C.radiod_api_control_set(cs, C.double(value))))
}

func (binding) Settings() Settings {
	var s C.radiod_api_settings
	C.radiod_api_settings_get(&s)
	return Settings{
		DefaultProfile: int(s.default_profile), DefaultProfileFallback: int(s.default_profile_fallback_s),
		StepHz: uint32(s.step_hz), SWRLimitX10: uint32(s.swr_limit_x10),
		HasBfo: bool(s.has_bfo), BfoHz: uint32(s.bfo_hz),
	}
}

func (binding) SetDefaultProfile(n int) error {
	return codeErr(int(C.radiod_api_set_default_profile(prof(n))))
}
func (binding) SetDefaultProfileFallback(s int) error {
	return codeErr(int(C.radiod_api_set_default_profile_fallback(C.int32_t(s))))
}
func (binding) SetStep(hz uint32) error { return codeErr(int(C.radiod_api_set_step(C.uint32_t(hz)))) }
func (binding) SetSWRLimit(v uint32) error {
	return codeErr(int(C.radiod_api_set_swr_limit(C.uint32_t(v))))
}
func (binding) SetBfo(hz uint32) error { return codeErr(int(C.radiod_api_set_bfo(C.uint32_t(hz)))) }

func (binding) DigiSettings() DigiSettings {
	var d C.radiod_api_digi_settings
	C.radiod_api_digi_settings_get(&d)
	return DigiSettings{
		CWWPM: int(d.cw_wpm), CWPitchHz: int(d.cw_pitch_hz), RTTYBaud: int(d.rtty_baud),
		RTTYShiftHz: int(d.rtty_shift_hz), RTTYMarkHz: int(d.rtty_mark_hz),
		DstarMycall: cstr(d.dstar_mycall[:]), DstarEncrypt: bool(d.dstar_encrypt),
		RadeEncrypt:  bool(d.rade_encrypt),
		DstarDenoise: bool(d.dstar_denoise), DstarVerbose: bool(d.dstar_verbose),
		VoiceCodec: cstr(d.voice_codec[:]),
	}
}

func (binding) SetDigi(key string, v int) error {
	cs := C.CString(key)
	defer C.free(unsafe.Pointer(cs))
	return codeErr(int(C.radiod_api_digi_set(cs, C.long(v))))
}

func (binding) SetVoiceCodec(name string) error {
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	return codeErr(int(C.radiod_api_set_voice_codec(cs)))
}

func (binding) DigiLog(after uint64, max int) []json.RawMessage {
	if max <= 0 {
		return nil
	}
	size := max*1600 + 1
	buf := C.malloc(C.size_t(size))
	defer C.free(buf)
	n := int(C.radiod_api_digi_log(C.uint64_t(after), C.int(max), (*C.char)(buf), C.size_t(size)))
	out := make([]json.RawMessage, 0, n)
	for _, line := range bytes.Split([]byte(C.GoString((*C.char)(buf))), []byte{'\n'}) {
		if len(line) > 0 {
			out = append(out, json.RawMessage(line))
		}
	}
	return out
}

func (binding) ModemStatus() (json.RawMessage, bool) {
	buf := C.malloc(4096)
	defer C.free(buf)
	if !bool(C.radiod_api_modem_status((*C.char)(buf), 4096)) {
		return nil, false
	}
	return json.RawMessage(C.GoString((*C.char)(buf))), true
}

// isoTime turns gpsd's unix seconds into the API's ISO 8601 UTC timestamp.
func isoTime(sec float64) string {
	nsec := int64((sec - float64(int64(sec))) * 1e9)
	return time.Unix(int64(sec), nsec).UTC().Format(time.RFC3339)
}

func (binding) Gps() Gps {
	var g C.radiod_api_gps
	C.radiod_api_gps_get(&g)
	out := Gps{Settings: GpsSettings{
		Logging:         bool(g.settings.logging),
		SampleIntervalS: int(g.settings.sample_interval_s),
		FilePeriodS:     int(g.settings.file_period_s),
		Report:          GpsReport{Enabled: bool(g.settings.report_enabled), IntervalS: int(g.settings.report_interval_s)},
	}}
	switch g.fix.fix {
	case 2:
		out.Fix = "2d"
	case 3:
		out.Fix = "3d"
	default:
		out.Fix = "none"
	}
	if bool(g.fix.has_position) {
		lat, lon := float64(g.fix.latitude), float64(g.fix.longitude)
		out.Latitude, out.Longitude = &lat, &lon
	}
	if bool(g.fix.has_altitude) {
		a := float64(g.fix.altitude_m)
		out.AltitudeM = &a
	}
	if bool(g.fix.has_time) {
		t := isoTime(float64(g.fix.time))
		out.At = &t
	}
	if bool(g.settings.has_email) {
		e := cstr(g.settings.email[:])
		out.Settings.Email = &e
	}
	return out
}

func (binding) SetGpsSettings(s GpsSettings) error {
	var cs C.radiod_api_gps_settings
	cs.logging = C.bool(s.Logging)
	cs.sample_interval_s = C.int32_t(s.SampleIntervalS)
	cs.file_period_s = C.int32_t(s.FilePeriodS)
	cs.report_enabled = C.bool(s.Report.Enabled)
	cs.report_interval_s = C.int32_t(s.Report.IntervalS)
	if s.Email != nil {
		cs.has_email = true
		// cs is zero-initialised, so the rest of the buffer stays NUL.
		for i := 0; i < len(cs.email)-1 && i < len(*s.Email); i++ {
			cs.email[i] = C.char((*s.Email)[i])
		}
	}
	return codeErr(int(C.radiod_api_gps_settings_set(&cs)))
}

func (binding) ListenMode() (string, error) {
	var buf [32]C.char
	if err := codeErr(int(C.radiod_api_modem_listen_mode(&buf[0], C.size_t(len(buf))))); err != nil {
		return "", err
	}
	return C.GoString(&buf[0]), nil
}

func (binding) SetListenMode(name string) error {
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	return codeErr(int(C.radiod_api_modem_set_listen_mode(cs)))
}

func (binding) ModemBusy() (string, error) {
	var buf [32]C.char
	if err := codeErr(int(C.radiod_api_modem_busy(&buf[0], C.size_t(len(buf))))); err != nil {
		return "", err
	}
	return C.GoString(&buf[0]), nil
}

func (binding) SetModemTxGain(db float64) error {
	return codeErr(int(C.radiod_api_modem_set_tx_gain(C.double(db))))
}

func (binding) SetModemWaterfall(on bool) error {
	return codeErr(int(C.radiod_api_modem_set_waterfall(C.bool(on))))
}

// chatBuf holds a few whole lines at least (mercury's longest is 8 KB).
const chatBuf = 64 << 10

func (binding) ChatSince(after uint64) ([]ChatLine, uint64, error) {
	buf := C.malloc(chatBuf)
	defer C.free(buf)
	var lines []ChatLine
	for {
		var last C.uint64_t
		n := C.radiod_api_modem_chat_since(C.uint64_t(after), (*C.char)(buf), chatBuf, &last)
		if n < 0 {
			return nil, after, codeErr(int(n))
		}
		if n == 0 || uint64(last) == after {
			return lines, after, nil
		}
		for _, l := range strings.Split(strings.TrimRight(C.GoStringN((*C.char)(buf), C.int(n)), "\n"), "\n") {
			var cl ChatLine
			if json.Unmarshal([]byte(l), &cl) == nil {
				lines = append(lines, cl)
			}
		}
		after = uint64(last)
	}
}

func (binding) ChatSend(plane, text string) error {
	cp, ct := C.CString(plane), C.CString(text)
	defer C.free(unsafe.Pointer(cp))
	defer C.free(unsafe.Pointer(ct))
	return codeErr(int(C.radiod_api_modem_chat_send(cp, ct)))
}

func (binding) WSSend(conn int64, f []byte) error {
	if len(f) == 0 {
		return nil
	}
	cs := C.CBytes(f)
	defer C.free(cs)
	if C.radiod_api_ws_send(C.long(conn), (*C.char)(cs), C.size_t(len(f))) != 0 {
		return ErrNotSupported
	}
	return nil
}

func (binding) WSClose(conn int64, code int, reason string) error {
	cr := C.CString(reason)
	defer C.free(unsafe.Pointer(cr))
	if C.radiod_api_ws_close(C.long(conn), C.int(code), cr) != 0 {
		return ErrNotSupported
	}
	return nil
}

func (binding) WSBroadcast(f []byte) error {
	if len(f) == 0 {
		return nil
	}
	cs := C.CBytes(f)
	defer C.free(cs)
	if C.radiod_api_ws_broadcast((*C.char)(cs), C.size_t(len(f))) != 0 {
		return ErrNotSupported
	}
	return nil
}

func (binding) Version() string { return C.GoString(C.radiod_api_version()) }

func (binding) CORSOrigins() string { return C.GoString(C.radiod_api_cors_origins()) }

// Log goes to radiod's logger through the hermes_log binding.
func (binding) Log(level LogLevel, component, message string) {
	hermeslog.Log(hermeslog.Level(level), component, message)
}
