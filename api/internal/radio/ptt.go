package radio

import (
	"context"
	"encoding/json/jsontext"
	"errors"
	"log/slog"
	"strconv"
	"time"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/logging"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/ratelimit"
	"github.com/Rhizomatica/hermes-radio-daemon/api/spec"
)

// PTTPerMinute is how many times a minute one user may key the transmitter
// (api-rest.yaml settings.tx_rate_limit.ptt_per_minute, its default; radiod
// has no station settings yet to change it).
const PTTPerMinute = 30

// ptt is radio.ptt (api-ws.yaml): the connection that keys owns PTT, only it
// unkeys, and radiod's C server unkeys when it closes (or when the station
// closes it because its session ended).
type ptt struct {
	r     rad.Radio
	limit *ratelimit.Limiter
	log   *slog.Logger
}

func registerPTT(rt *api.Router) {
	p := &ptt{r: rt.Radio(), limit: &ratelimit.Limiter{}, log: logging.New(rt.Radio(), "api.tx")}
	rt.Command("radio.ptt", spec.RoleOperator, p.cmd)
	rt.OnClose(p.closed)
}

// closed unkeys for a connection that closed holding PTT. radiod's C server
// unkeys on close already; this catches a radio.ptt on that was still
// running then, which would otherwise leave the transmitter keyed by a
// connection that no longer exists.
func (p *ptt) closed(c *api.Conn) {
	if p.r.PTT(c.ID, false) == nil {
		p.log.Info("radio.tx off: the connection closed holding PTT", "user", c.User, "conn", c.ID)
	}
}

func (p *ptt) cmd(_ context.Context, c *api.Conn, args jsontext.Value) (any, *api.CmdError) {
	var a struct {
		On *bool `json:"on"`
	}
	if cerr := api.DecodeArgs(args, &a); cerr != nil {
		return nil, cerr
	}
	if a.On == nil {
		return nil, api.Errf(api.CodeMissingArg, "on")
	}
	st := p.r.State()
	if !*a.On {
		if err := p.r.PTT(c.ID, false); err != nil {
			return nil, pttErr(err)
		}
		p.log.Info("radio.tx off", "user", c.User, "conn", c.ID)
		return map[string]bool{"on": false}, nil
	}
	ok, retry, _ := p.limit.Reserve("ptt:"+strconv.FormatInt(c.User, 10), PTTPerMinute, time.Minute)
	if !ok {
		return nil, api.Errf(api.CodeRateLimited, "keyed %d times this minute; again in %d s",
			PTTPerMinute, int(retry.Seconds())+1)
	}
	if err := p.r.PTT(c.ID, true); err != nil {
		return nil, pttErr(err)
	}
	// The audit trail of who made the station transmit (api-ws.yaml
	// radio.tx), until radiod has its audit log table.
	p.log.Info("radio.tx on", "user", c.User, "conn", c.ID,
		"frequency_hz", st.FrequencyHz, "mode", st.Mode, "encrypted", encryptedVoice(st, p.r.DigiSettings()))
	return map[string]bool{"on": true}, nil
}

// encryptedVoice reports whether keying now sends encrypted voice (for the
// log): the profile in digital voice with its codec's encryption on.
func encryptedVoice(st rad.State, d rad.DigiSettings) bool {
	if st.Mode == "DSTAR" {
		return d.DstarEncrypt
	}
	return st.DigitalVoice && d.VoiceCodec == "RADEV2" && d.RadeEncrypt
}

func pttErr(err error) *api.CmdError {
	switch {
	case errors.Is(err, rad.ErrBusy):
		return api.Errf(api.CodeBusy, "another client holds PTT")
	case errors.Is(err, rad.ErrProtection):
		return api.Errf(api.CodeBusy, "the SWR protection has tripped: radio.reset_protection first")
	case errors.Is(err, rad.ErrNoPTT):
		return api.Errf(api.CodeNoPTT, "this connection does not hold PTT")
	default:
		return cmdErr(err)
	}
}
