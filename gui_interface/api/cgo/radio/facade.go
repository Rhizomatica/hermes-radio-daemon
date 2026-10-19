//go:build radiod

package radio

/*
#cgo CFLAGS: -I${SRCDIR}/../../..
// The facade's symbols live in radio_daemon, linked after this archive;
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
)

// Default is radiod's facade.
func Default() Radio { return facade{} }

type facade struct{}

func cstr(b []C.char) string { return C.GoString(&b[0]) }

func (facade) State() State {
	var s C.radiod_api_state
	C.radiod_api_state_get(&s)
	return State{
		ActiveProfile: int(s.active_profile), TX: bool(s.tx), FrequencyHz: uint32(s.frequency_hz),
		Mode: cstr(s.mode[:]), Power: int(s.power), FwdW: float64(s.fwd_w), RefW: float64(s.ref_w),
		SWR: float64(s.swr), SMeterValid: bool(s.s_meter_valid), SMeterDB: float64(s.s_meter_db),
		Protection: bool(s.protection), TimeoutS: int(s.timeout_s), DigitalVoice: bool(s.digital_voice),
		Backend: cstr(s.backend[:]),
	}
}

func (facade) ProfileCount() int { return int(C.radiod_api_profile_count()) }

func (facade) Profile(n int) (Profile, error) {
	if n < 0 {
		return Profile{}, ErrNoProfile
	}
	var p C.radiod_api_profile
	if err := codeErr(int(C.radiod_api_profile_get(C.uint32_t(n), &p))); err != nil {
		return Profile{}, err
	}
	return Profile{
		Profile: int(p.profile), FrequencyHz: uint32(p.frequency_hz), Mode: cstr(p.mode[:]),
		Power: int(p.power), Volume: int(p.volume), StepHz: uint32(p.step_hz),
		FilterWidthHz: uint32(p.filter_width_hz), TimeoutS: int(p.timeout_s),
		RefThreshold: uint32(p.ref_threshold), BfoHz: int(p.bfo_hz), DigitalVoice: bool(p.digital_voice),
	}, nil
}

func prof(n int) C.uint32_t {
	if n < 0 {
		return C.uint32_t(^uint32(0)) // never a profile: ENOENT
	}
	return C.uint32_t(n)
}

func (facade) SelectProfile(n int) error { return codeErr(int(C.radiod_api_select_profile(prof(n)))) }
func (facade) SetFrequency(n int, hz uint32) error {
	return codeErr(int(C.radiod_api_set_frequency(prof(n), C.uint32_t(hz))))
}
func (facade) SetMode(n int, mode string) error {
	cs := C.CString(mode)
	defer C.free(unsafe.Pointer(cs))
	return codeErr(int(C.radiod_api_set_mode(prof(n), cs)))
}
func (facade) SetPower(n int, v int) error {
	return codeErr(int(C.radiod_api_set_power(prof(n), C.int(v))))
}
func (facade) SetVolume(n int, v int) error {
	return codeErr(int(C.radiod_api_set_volume(prof(n), C.int(v))))
}
func (facade) SetFilterWidth(n int, hz uint32) error {
	return codeErr(int(C.radiod_api_set_filter_width(prof(n), C.uint32_t(hz))))
}
func (facade) SetDigitalVoice(n int, on bool) error {
	return codeErr(int(C.radiod_api_set_digital_voice(prof(n), C.bool(on))))
}
func (facade) SetStep(hz uint32) error { return codeErr(int(C.radiod_api_set_step(C.uint32_t(hz)))) }
func (facade) SetTimeout(s int) error  { return codeErr(int(C.radiod_api_set_timeout(C.int32_t(s)))) }
func (facade) SetRefThreshold(v uint32) error {
	return codeErr(int(C.radiod_api_set_ref_threshold(C.uint32_t(v))))
}
func (facade) SetBfo(hz int) error { return codeErr(int(C.radiod_api_set_bfo(C.int32_t(hz)))) }
func (facade) ResetProtection()    { C.radiod_api_reset_protection() }

func (facade) DigiSettings() DigiSettings {
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

func (facade) SetDigi(key string, v int) error {
	cs := C.CString(key)
	defer C.free(unsafe.Pointer(cs))
	return codeErr(int(C.radiod_api_digi_set(cs, C.long(v))))
}

func (facade) SetVoiceCodec(name string) error {
	cs := C.CString(name)
	defer C.free(unsafe.Pointer(cs))
	return codeErr(int(C.radiod_api_set_voice_codec(cs)))
}

func (facade) DigiLog(after uint64, max int) []json.RawMessage {
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

func (facade) ModemStatus() (json.RawMessage, bool) {
	buf := C.malloc(4096)
	defer C.free(buf)
	if !bool(C.radiod_api_modem_status((*C.char)(buf), 4096)) {
		return nil, false
	}
	return json.RawMessage(C.GoString((*C.char)(buf))), true
}

func (facade) WSSend(conn int64, f []byte) error {
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

func (facade) WSBroadcast(f []byte) error {
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

func (facade) Version() string { return C.GoString(C.radiod_api_version()) }
