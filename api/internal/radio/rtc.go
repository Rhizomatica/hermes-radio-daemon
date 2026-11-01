package radio

import (
	"context"
	"encoding/json/jsontext"
	"errors"
	"log/slog"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/logging"
	"github.com/Rhizomatica/hermes-radio-daemon/api/spec"
)

// SDPMax is the longest offer rtc.offer takes (api-ws.yaml RtcOfferArgs).
const SDPMax = 16384

// rtc is rtc.offer and rtc.close (api-ws.yaml, Audio): this socket signals
// the connection's WebRTC audio, which radiod's C side negotiates and
// carries. The session ends with the socket.
type rtc struct {
	r   rad.Radio
	log *slog.Logger
}

func registerRTC(rt *api.Router) {
	h := &rtc{r: rt.Radio(), log: logging.New(rt.Radio(), "api.rtc")}
	rt.Command("rtc.offer", spec.RoleUser, h.offer)
	rt.Command("rtc.close", spec.RoleUser, h.close)
	rt.OnClose(h.closed)
}

// closed ends the session of a connection that closed: radiod's C server
// does on close already, and this catches an rtc.offer that was still
// running then.
func (h *rtc) closed(c *api.Conn) { h.r.RTCClose(c.ID) }

func (h *rtc) offer(_ context.Context, c *api.Conn, args jsontext.Value) (any, *api.CmdError) {
	a := struct {
		SDP *string `json:"sdp"`
		RX  *bool   `json:"rx"`
		TX  *bool   `json:"tx"`
	}{}
	if cerr := api.DecodeArgs(args, &a); cerr != nil {
		return nil, cerr
	}
	if a.SDP == nil || *a.SDP == "" {
		return nil, api.Errf(api.CodeMissingArg, "sdp")
	}
	if len(*a.SDP) > SDPMax {
		return nil, api.Errf(api.CodeOutOfRange, "sdp: longer than %d bytes", SDPMax)
	}
	rx, tx := a.RX == nil || *a.RX, a.TX != nil && *a.TX
	if tx && !api.AtLeast(c.Role, spec.RoleOperator) {
		return nil, api.Errf(api.CodeForbidden, "tx: sending microphone audio needs operator")
	}
	if !rx && !tx {
		return nil, api.Errf(api.CodeOutOfRange, "rx, tx: one of them must be true")
	}
	answer, err := h.r.RTCOffer(c.ID, *a.SDP, rx, tx)
	if err != nil {
		return nil, rtcErr(err)
	}
	h.log.Info("rtc.offer", "user", c.User, "conn", c.ID, "rx", rx, "tx", tx)
	return map[string]string{"sdp": answer}, nil
}

func (h *rtc) close(_ context.Context, c *api.Conn, args jsontext.Value) (any, *api.CmdError) {
	var a struct{}
	if cerr := api.DecodeArgs(args, &a); cerr != nil {
		return nil, cerr
	}
	h.r.RTCClose(c.ID)
	return nil, nil
}

func rtcErr(err error) *api.CmdError {
	switch {
	case errors.Is(err, rad.ErrInvalid):
		return api.Errf(api.CodeBadRequest, "sdp: the offer has no Opus audio section")
	case errors.Is(err, rad.ErrBusy):
		return api.Errf(api.CodeBusy, "every audio session is taken")
	case errors.Is(err, rad.ErrIO):
		return api.Errf(api.CodeInternal, "the audio session could not be negotiated")
	case errors.Is(err, rad.ErrNotSupported):
		return api.Errf(api.CodeNotSupported, "this station has no browser audio")
	default:
		return cmdErr(err)
	}
}
