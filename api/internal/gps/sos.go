package gps

// The SOS over UUCP and NNCP (docs/api-design/store-and-forward.md, "The
// SOS, the first user"): besides the beacons on the air, the compact beacon
// goes to each station of sos_recipients through its dec_beacon handler,
// and the station calls at once, so the alarm leaves even where the
// recipient doesn't hear the beacons.

import (
	"context"
	"encoding/json/jsontext"
	"encoding/json/v2"
	"errors"
	"strconv"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/transport"
)

// SosRecipientsMax is the most stations an SOS goes to (core.ini
// main:sos_recipients, radio.h RADIO_SOS_RECIPIENTS_LEN).
const SosRecipientsMax = 8

// SosHandler is the handler that takes a beacon at the other station
// (handlers/dec_beacon.c).
const SosHandler = "dec_beacon"

// recipients reads sos_recipients: station names the transports reach, at
// most SosRecipientsMax, no repeats.
func (h handlers) recipients(v jsontext.Value, key string, f api.Fields) ([]string, bool) {
	var list []string
	if json.Unmarshal(v, &list) != nil || len(list) > SosRecipientsMax {
		f.Add(key, "a list of up to "+strconv.Itoa(SosRecipientsMax)+" station names")
		return nil, false
	}
	l := transport.ForRadio(h.r)
	seen := map[string]bool{}
	for _, n := range list {
		if _, st, _, err := l.Route(n); err != nil {
			f.Add(key, n+": not a station UUCP or NNCP reaches")
			return nil, false
		} else if seen[st.Name] {
			f.Add(key, n+": listed twice")
			return nil, false
		} else {
			seen[st.Name] = true
		}
	}
	return list, true
}

// sosQueue sends the beacon with code to every SOS recipient over the
// transport that reaches it, then calls. Failures are logged: the beacons
// on the air go out regardless.
func (h handlers) sosQueue(code int) {
	list := h.r.Beacon().SosRecipients
	if len(list) == 0 {
		return
	}
	frame, err := h.r.BeaconFrame(code)
	if err != nil {
		h.log.Error("SOS not queued to its recipients", "code", code, "error", err)
		return
	}
	l := transport.ForRadio(h.r)
	queued := []string{}
	for _, dest := range list {
		job, err := l.Exec(context.Background(), dest, SosHandler, nil, frame)
		if err != nil {
			h.log.Error("SOS not queued", "station", dest, "code", code, "error", err)
			continue
		}
		h.log.Warn("SOS queued", "station", dest, "code", code, "job", job)
		queued = append(queued, dest)
	}
	call := ""
	switch len(queued) {
	case 0:
		return
	case 1:
		call = queued[0]
	}
	// One recipient is called by name; several, by calling every station
	// with work queued, since one call runs at a time.
	if _, err := l.Call(call, ""); errors.Is(err, transport.ErrBusy) {
		h.log.Warn("SOS waits for the call in progress", "code", code)
	} else if err != nil {
		h.log.Error("SOS queued but not called", "code", code, "error", err)
	}
}
