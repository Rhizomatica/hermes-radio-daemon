package gps

// The SOS over UUCP and NNCP (docs/api-design/store-and-forward.md, "The
// SOS, the first user"): besides the beacons on the air, the compact beacon
// goes to each station of sos_recipients through its dec_beacon handler,
// and the station calls at once, so the alarm leaves even where the
// recipient doesn't hear the beacons, and on a station without the
// broadcast plane.

import (
	"context"
	"encoding/json/jsontext"
	"encoding/json/v2"
	"errors"
	"strconv"
	"sync"
	"time"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/transport"
)

const (
	// SosRecipientsMax is the most stations an SOS goes to (core.ini
	// main:sos_recipients, radio.h RADIO_SOS_RECIPIENTS_LEN).
	SosRecipientsMax = 8

	// SosHandler is the handler that takes a beacon at the other station
	// (handlers/dec_beacon.c).
	SosHandler = "dec_beacon"
)

// SosCallPoll is how often an SOS waiting for the call in progress looks
// again, and SosCallWaitMax how long it waits in all before it leaves its
// jobs for the next scheduled call. Variables for the tests.
var (
	SosCallPoll    = 10 * time.Second
	SosCallWaitMax = time.Hour
)

// sosState orders the SOS's steps. set: an emergency's read and change are
// one step, so two clears at once send one "emergency over". queue: jobs
// are queued in the order the codes were set. call: one SOS calls at a
// time, since one call runs at a time on the station's link.
type sosState struct {
	set, queue, call sync.Mutex
}

// recipients reads sos_recipients: station names the transports reach, at
// most SosRecipientsMax, no repeats.
func (h handlers) recipients(v jsontext.Value, key string, f api.Fields) ([]string, bool) {
	var list []string
	if json.Unmarshal(v, &list) != nil || len(list) > SosRecipientsMax {
		f.Add(key, "a list of up to "+strconv.Itoa(SosRecipientsMax)+" station names")
		return nil, false
	}
	l := transport.ForRadio(h.r).Snapshot()
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

// sosQueue sends the beacon with code, set at sent, to every SOS recipient
// over the transport that reaches it, then calls them. The handler gets the
// time it was set, so the recipient applies an SOS and its "emergency over"
// in that order, however they arrive. Failures are logged: the beacons on
// the air go out regardless.
func (h handlers) sosQueue(code int, sent time.Time) {
	h.sos.queue.Lock()
	queued, l := h.sosJobs(code, sent)
	h.sos.queue.Unlock()
	if len(queued) > 0 {
		h.sosCall(l, code, queued)
	}
}

// sosJobs queues the jobs and returns the stations they went to, and the
// layer that reaches them.
func (h handlers) sosJobs(code int, sent time.Time) ([]string, *transport.Layer) {
	list := h.r.Beacon().SosRecipients
	if len(list) == 0 {
		return nil, nil
	}
	frame, err := h.r.BeaconFrame(code)
	if err != nil {
		h.log.Error("SOS not queued to its recipients", "code", code, "error", err)
		return nil, nil
	}
	l := transport.ForRadio(h.r).Snapshot()
	args := []string{strconv.FormatInt(sent.UnixMilli(), 10)}
	queued := []string{}
	for _, dest := range list {
		job, err := l.Exec(context.Background(), dest, SosHandler, args, frame)
		if err != nil {
			h.log.Error("SOS not queued", "station", dest, "code", code, "error", err)
			continue
		}
		h.log.Warn("SOS queued", "station", dest, "code", code, "job", job)
		queued = append(queued, dest)
	}
	return queued, l
}

// sosCall calls each station in turn, by name, so only the SOS's transport
// runs for it and no unrelated station is called. A call in progress (one
// at a time on the link, this SOS's previous one included) is waited for,
// up to SosCallWaitMax; past it the jobs wait for the next scheduled call.
func (h handlers) sosCall(l *transport.Layer, code int, dests []string) {
	h.sos.call.Lock()
	defer h.sos.call.Unlock()
	end := time.Now().Add(SosCallWaitMax)
	for _, dest := range dests {
		for {
			_, err := l.Call(dest, "")
			if err == nil {
				break
			}
			if !errors.Is(err, transport.ErrBusy) {
				h.log.Error("SOS queued but not called", "station", dest, "code", code, "error", err)
				break
			}
			if time.Now().After(end) {
				h.log.Error("SOS queued, but the calls in progress went on too long; it waits for the next call",
					"code", code, "waited", SosCallWaitMax.String())
				return
			}
			time.Sleep(SosCallPoll)
		}
	}
}
