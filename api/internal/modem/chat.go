package modem

import (
	"context"
	"encoding/json"
	"errors"
	"time"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
)

// chatSnapshot is how many of the lines mercury holds (up to 500) a
// subscriber gets first: as digi's, enough to fill a screen, and well
// inside a client's send queue.
const chatSnapshot = 100

// ChatLine is api-ws.yaml's ChatLine.
type ChatLine struct {
	Plane string  `json:"plane"` // arq, broadcast
	Dir   string  `json:"dir"`   // rx, tx
	Peer  *string `json:"peer"`
	Text  string  `json:"text"`
	At    string  `json:"at"`
}

func chatLineOf(l rad.ChatLine) ChatLine {
	plane := l.Plane
	if plane == "bcast" {
		plane = "broadcast"
	}
	at := time.Unix(l.TS, int64(l.MS)*int64(time.Millisecond)).UTC()
	return ChatLine{Plane: plane, Dir: l.Dir, Peer: str(l.Peer), Text: l.Text,
		At: at.Format("2006-01-02T15:04:05.000Z")}
}

// chat reads modem.chat: the lines after seq after; the snapshot (after 0)
// keeps the last chatSnapshot.
func (h handlers) chat(after uint64) ([]any, uint64, error) {
	lines, next, err := h.r.ChatSince(after)
	if err != nil {
		return nil, after, err
	}
	if after == 0 && len(lines) > chatSnapshot {
		lines = lines[len(lines)-chatSnapshot:]
	}
	items := make([]any, len(lines))
	for i, l := range lines {
		items[i] = chatLineOf(l)
	}
	return items, next, nil
}

// modem.chat.send: one line on either plane; the broadcast plane sends in
// the listen mode. The line comes back on modem.chat once mercury stores it.
func (h handlers) cmdChatSend(_ context.Context, _ *api.Conn, args json.RawMessage) (any, *api.CmdError) {
	var a struct {
		Plane *string `json:"plane"`
		Text  *string `json:"text"`
	}
	if cerr := api.DecodeArgs(args, &a); cerr != nil {
		return nil, cerr
	}
	switch {
	case a.Plane == nil:
		return nil, api.Errf(api.CodeMissingArg, "plane")
	case a.Text == nil:
		return nil, api.Errf(api.CodeMissingArg, "text")
	case *a.Plane != "arq" && *a.Plane != "broadcast":
		return nil, api.Errf(api.CodeOutOfRange, "plane: arq or broadcast")
	case *a.Text == "" || len(*a.Text) > 1024:
		return nil, api.Errf(api.CodeOutOfRange, "text: 1 to 1024 bytes")
	}
	err := h.r.ChatSend(*a.Plane, *a.Text)
	switch {
	case err == nil:
		return nil, nil
	case errors.Is(err, rad.ErrNoLink):
		return nil, api.Errf(api.CodeBadRequest, "no ARQ session is up")
	case errors.Is(err, rad.ErrBusy):
		return nil, api.Errf(api.CodeQueueFull, "the ARQ session's queue is full")
	case errors.Is(err, rad.ErrTooLong):
		return nil, api.Errf(api.CodeOutOfRange, "text: longer than one broadcast frame in the listen mode")
	case errors.Is(err, rad.ErrNotSupported):
		return nil, api.Errf(api.CodeNotSupported, "mercury is not running in this daemon")
	default:
		return nil, api.Errf(api.CodeOutOfRange, "%v", err)
	}
}
