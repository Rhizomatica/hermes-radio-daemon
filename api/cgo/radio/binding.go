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

// Log goes to radiod's logger through the hermes_log binding.
func (binding) Log(level LogLevel, component, message string) {
	hermeslog.Log(hermeslog.Level(level), component, message)
}
