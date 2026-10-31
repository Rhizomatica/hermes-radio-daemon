// Package modem serves mercury's REST operations (api-rest.yaml, tag
// modem) and hermes.v1 topics and commands: its status as mercury
// publishes it inside radiod, and its configuration, listen mode and chat
// through mercury's host API.
package modem

import (
	"encoding/json/v2"
	"errors"
	"fmt"
	"net/http"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/spec"
)

// Register adds mercury's operations and topics.
func Register(rt *api.Router) {
	h := handlers{rt.Radio()}
	rt.Op("getModemStatus", h.getStatus)
	rt.Op("getModemConfig", h.getConfig)
	rt.Op("updateModemConfig", h.updateConfig)

	// The same ModemStatus as getModemStatus, pushed as it changes; not
	// offered while mercury is not running. Likewise ModemConfig.
	rt.Topic("modem.status", spec.RoleReadonly, func() (any, error) { return h.status() })
	rt.Topic("modem.config", spec.RoleOperator, func() (any, error) { return h.config() })

	// Mercury's chat, both planes: the last chatSnapshot lines it holds on
	// subscribe, then each line as it is stored (sent or received).
	rt.Stream("modem.chat", spec.RoleReadonly, h.chat)
	rt.Command("modem.chat.send", spec.RoleUser, h.cmdChatSend)
}

// errNotRunning: radiod has no mercury status (built without mercury, not
// enabled, or still starting).
var errNotRunning = errors.New("mercury is not running in this daemon")

// status reads mercury's last status as ModemStatus.
func (h handlers) status() (Status, error) {
	raw, ok := h.r.ModemStatus()
	if !ok {
		return Status{}, errNotRunning
	}
	var m mercuryStatus
	if err := json.Unmarshal(raw, &m, rad.FromC); err != nil {
		return Status{}, fmt.Errorf("mercury's status: %w", err)
	}
	return statusOf(m), nil
}

type handlers struct{ r rad.Radio }

// mercuryStatus is mercury's own {"type":"status",...} (its
// docs/WEBSOCKET_API.md), what radiod keeps of it.
type mercuryStatus struct {
	Bitrate      int      `json:"bitrate"`
	SNR          float64  `json:"snr"`
	UserCallsign string   `json:"user_callsign"`
	DestCallsign string   `json:"dest_callsign"`
	Sync         bool     `json:"sync"`
	Direction    string   `json:"direction"`
	BytesTx      int64    `json:"bytes_transmitted"`
	BytesRx      int64    `json:"bytes_received"`
	TxGainDB     float64  `json:"tx_gain_db"`
	TxPeakDBFS   *float64 `json:"tx_peak_dbfs"`
	AudioOK      bool     `json:"audio_ok"`
	AudioError   string   `json:"audio_error"`
	ArqTxMode    string   `json:"arq_tx_mode"`
	ArqRxMode    string   `json:"arq_rx_mode"`
	PeerSNR      float64  `json:"peer_snr"`
	PeerSNRValid bool     `json:"peer_snr_valid"`
	Waterfall    bool     `json:"waterfall"`
	ListenMode   string   `json:"listen_mode_name"`
	Broadcast    *struct {
		State       string `json:"state"` // idle sending stopped_by_arq stopped done error
		Cycle       int    `json:"cycle"`
		CyclesTotal int    `json:"cycles_total"`
		FramesSent  int64  `json:"frames_sent"`
	} `json:"broadcast"`
}

// Status is ModemStatus.
type Status struct {
	Link             string   `json:"link"`
	MyCall           *string  `json:"my_call"`
	PeerCall         *string  `json:"peer_call"`
	Direction        string   `json:"direction"`
	SNRDB            *float64 `json:"snr_db"`
	PeerSNRDB        *float64 `json:"peer_snr_db"`
	BitrateBps       *int     `json:"bitrate_bps"`
	BytesTx          int64    `json:"bytes_tx"`
	BytesRx          int64    `json:"bytes_rx"`
	TxGainDB         float64  `json:"tx_gain_db"`
	TxPeakDBFS       *float64 `json:"tx_peak_dbfs"`
	ArqTxMode        *string  `json:"arq_tx_mode"`
	ArqRxMode        *string  `json:"arq_rx_mode"`
	SessionTransport *string  `json:"session_transport"`
	ListenMode       *string  `json:"listen_mode"`
	Broadcast        Plane    `json:"broadcast"`
	AudioOK          bool     `json:"audio_ok"`
	AudioError       *string  `json:"audio_error"`
}

// Plane is ModemStatus.broadcast: mercury's carousel. radiod has no
// broadcast messages yet, so message_id and frames_total are null.
type Plane struct {
	State       string  `json:"state"` // idle, sending, preempted
	MessageID   *int64  `json:"message_id"`
	Mode        *string `json:"mode"`
	Cycle       *int    `json:"cycle"`
	CyclesTotal *int    `json:"cycles_total"`
	FramesSent  *int64  `json:"frames_sent"`
	FramesTotal *int64  `json:"frames_total"`
}

// listenModes are api-rest.yaml's BroadcastMode: the modes mercury can
// switch to at run time.
var listenModes = map[string]bool{"DATAC1": true, "DATAC3": true, "DATAC4": true, "DATAC15": true,
	"DATAC17": true, "QAM16C2": true, "MFSK": true, "MFSK500": true}

// listenMode is mercury's listen mode as a BroadcastMode, or null.
func listenMode(name string) *string {
	if !listenModes[name] {
		return nil
	}
	return &name
}

// planeOf maps mercury's carousel onto ModemStatus.broadcast. Mercury sends
// at its listen mode; a carousel an ARQ session stopped is preempted until
// radiod sends it again; one stopped, done or failed leaves the plane idle.
func planeOf(m mercuryStatus) Plane {
	p := Plane{State: "idle"}
	b := m.Broadcast
	if b == nil {
		return p
	}
	switch b.State {
	case "sending":
		p.State = "sending"
	case "stopped_by_arq":
		p.State = "preempted"
	default:
		return p
	}
	cycle, total, sent := b.Cycle, b.CyclesTotal, b.FramesSent
	p.Mode, p.Cycle, p.FramesSent = listenMode(m.ListenMode), &cycle, &sent
	if total > 0 { // 0: until stopped
		p.CyclesTotal = &total
	}
	return p
}

func str(s string) *string {
	if s == "" {
		return nil
	}
	return &s
}

// statusOf maps mercury's status onto ModemStatus. Mercury's status has no
// link state of its own: "connected" while it is synchronised with a peer,
// "idle" otherwise. The session transport and listen mode are not in it.
func statusOf(m mercuryStatus) Status {
	// A mode is null until data has gone that way in the session: mercury
	// sends "" until then (a carousel side that only polled has no TX mode).
	s := Status{
		Link: "idle", MyCall: str(m.UserCallsign), PeerCall: str(m.DestCallsign),
		Direction: m.Direction, BytesTx: m.BytesTx, BytesRx: m.BytesRx,
		TxGainDB: m.TxGainDB, TxPeakDBFS: m.TxPeakDBFS,
		ArqTxMode: str(m.ArqTxMode), ArqRxMode: str(m.ArqRxMode),
		ListenMode: listenMode(m.ListenMode), Broadcast: planeOf(m),
		AudioOK: m.AudioOK, AudioError: str(m.AudioError),
	}
	if m.Sync && m.DestCallsign != "" {
		s.Link = "connected"
		snr, br := m.SNR, m.Bitrate
		s.SNRDB, s.BitrateBps = &snr, &br
	}
	if m.PeerSNRValid {
		p := m.PeerSNR
		s.PeerSNRDB = &p
	}
	if s.Direction != "tx" && s.Direction != "rx" {
		s.Direction = "idle"
	}
	return s
}

func (h handlers) getStatus(w http.ResponseWriter, r *http.Request) {
	s, err := h.status()
	switch {
	case errors.Is(err, errNotRunning):
		api.Problem(w, r, http.StatusServiceUnavailable, "not_running", err.Error(), nil)
	case err != nil:
		api.Problem(w, r, http.StatusInternalServerError, "internal", err.Error(), nil)
	default:
		api.JSON(w, http.StatusOK, s)
	}
}
