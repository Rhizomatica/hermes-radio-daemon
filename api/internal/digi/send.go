package digi

import (
	"context"
	"encoding/json/jsontext"
	"encoding/json/v2"
	"errors"
	"log/slog"
	"strconv"
	"strings"
	"time"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/logging"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/ratelimit"
	"github.com/Rhizomatica/hermes-radio-daemon/api/spec"
)

// DigiPerMinute is how many digi.send one user may queue a minute
// (api-rest.yaml settings.tx_rate_limit.digi_per_minute, its default).
const DigiPerMinute = 6

// snapshot bounds how many digi log entries a subscription to digi starts
// with. radiod's ring keeps 64 (DIGI_LOG_RING), so in practice the snapshot
// is all of it (api-ws.yaml); older entries are in GET /digi/log.
const snapshot = 100

// textMax is digi.send's text limit (api-ws.yaml DigiSendArgs) and what
// radiod's queue holds once " de <callsign>" is added (DIGI_TX_MSG_MAX - 1).
const textMax = 255

type sender struct {
	rt    *api.Router
	r     rad.Radio
	limit *ratelimit.Limiter
	log   *slog.Logger
}

func registerSend(rt *api.Router) {
	s := &sender{rt: rt, r: rt.Radio(), limit: &ratelimit.Limiter{}, log: logging.New(rt.Radio(), "api.tx")}
	rt.Command("digi.send", spec.RoleOperator, s.cmd)
	// Every decode, line and header, and every transmission: the last 100 on
	// subscribe, then each entry as radiod logs it.
	rt.Stream("digi", spec.RoleReadonly, s.stream)
}

// digi.send: text for the active profile's digital mode (FT8, CW or RTTY).
// CW and RTTY identify the station: without its callsign the text gets
// " de <callsign>". The reply says it is queued; the digi topic reports it
// (dir tx) when it goes out.
func (s *sender) cmd(ctx context.Context, c *api.Conn, args jsontext.Value) (any, *api.CmdError) {
	var a struct {
		Mode *string `json:"mode"`
		Text *string `json:"text"`
	}
	if cerr := api.DecodeArgs(args, &a); cerr != nil {
		return nil, cerr
	}
	if a.Text == nil {
		return nil, api.Errf(api.CodeMissingArg, "text")
	}
	text := strings.TrimSpace(*a.Text)
	if text == "" || len(text) > 256 {
		return nil, api.Errf(api.CodeOutOfRange, "text: 1 to 256 characters")
	}
	if a.Mode != nil && *a.Mode != "FT8" && *a.Mode != "CW" && *a.Mode != "RTTY" {
		return nil, api.Errf(api.CodeOutOfRange, "mode: FT8, CW or RTTY")
	}
	st := s.r.State()
	if st.Mode != "FT8" && st.Mode != "CW" && st.Mode != "RTTY" {
		return nil, api.Errf(api.CodeWrongMode, "the active profile is in %s; set FT8, CW or RTTY with radio.set first", st.Mode)
	}
	if a.Mode != nil && *a.Mode != st.Mode {
		return nil, api.Errf(api.CodeWrongMode, "the active profile is in %s, not %s", st.Mode, *a.Mode)
	}
	if st.Mode != "FT8" {
		if call := s.callsign(ctx); call != "" && !strings.Contains(strings.ToUpper(text), call) {
			text += " de " + call
		}
	}
	if len(text) > textMax {
		return nil, api.Errf(api.CodeOutOfRange, "text: too long with the station's callsign added")
	}
	ok, retry, _ := s.limit.Reserve("digi:"+strconv.FormatInt(c.User, 10), DigiPerMinute, time.Minute)
	if !ok {
		return nil, api.Errf(api.CodeRateLimited, "sent %d this minute; again in %d s", DigiPerMinute, int(retry.Seconds())+1)
	}
	if err := s.r.DigiSend(text); err != nil {
		switch {
		case errors.Is(err, rad.ErrInvalid):
			return nil, api.Errf(api.CodeOutOfRange, "not a valid FT8 message (free text: 13 characters, A-Z 0-9 space + - . / ?)")
		case errors.Is(err, rad.ErrBusy):
			return nil, api.Errf(api.CodeQueueFull, "the transmit queue is full")
		case errors.Is(err, rad.ErrNotSupported):
			return nil, api.Errf(api.CodeWrongMode, "the active profile is not in FT8, CW or RTTY")
		default:
			return nil, api.Errf(api.CodeInternal, "%v", err)
		}
	}
	s.log.Info("radio.tx digi", "user", c.User, "conn", c.ID, "mode", st.Mode,
		"frequency_hz", st.FrequencyHz, "chars", len(text))
	return map[string]any{"queued": true, "text": text}, nil
}

// callsign is the station's, upper case: the one setup recorded, or radiod's
// dstar_mycall; "" when it has none.
func (s *sender) callsign(ctx context.Context) string {
	if s.rt.StationCallsign != nil {
		if c := strings.TrimSpace(s.rt.StationCallsign(ctx)); c != "" {
			return strings.ToUpper(c)
		}
	}
	if c := strings.ToUpper(strings.TrimSpace(s.r.DigiSettings().DstarMycall)); c != "" && c != "N0CALL" {
		return c
	}
	return ""
}

// stream reads the digi log after an entry id: on subscribe (after 0) its
// last snapshot entries.
func (s *sender) stream(after uint64) ([]any, uint64, error) {
	entries := s.r.DigiLog(after, ringMax)
	if after == 0 && len(entries) > snapshot {
		entries = entries[len(entries)-snapshot:]
	}
	items := make([]any, len(entries))
	next := after
	for i, e := range entries {
		items[i] = e
		var h entryHead
		if json.Unmarshal(e, &h) == nil && h.ID > next {
			next = h.ID
		}
	}
	return items, next, nil
}
