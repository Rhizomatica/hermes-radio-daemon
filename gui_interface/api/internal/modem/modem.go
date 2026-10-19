// Package modem serves mercury's REST operations (api-rest.yaml, tag
// modem), from the status mercury publishes inside radiod.
package modem

import (
	"encoding/json"
	"net/http"

	rad "github.com/Rhizomatica/hermes-radio-daemon/gui_interface/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/gui_interface/api/internal/api"
)

// Register adds mercury's operations.
func Register(rt *api.Router) {
	h := handlers{rt.Radio()}
	rt.Op("getModemStatus", h.getStatus)
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
	AudioOK          bool     `json:"audio_ok"`
	AudioError       *string  `json:"audio_error"`
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
	s := Status{
		Link: "idle", MyCall: str(m.UserCallsign), PeerCall: str(m.DestCallsign),
		Direction: m.Direction, BytesTx: m.BytesTx, BytesRx: m.BytesRx,
		TxGainDB: m.TxGainDB, TxPeakDBFS: m.TxPeakDBFS,
		ArqTxMode: str(m.ArqTxMode), ArqRxMode: str(m.ArqRxMode),
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
	raw, ok := h.r.ModemStatus()
	if !ok {
		api.Problem(w, r, http.StatusServiceUnavailable, "not_running",
			"mercury is not running in this daemon", nil)
		return
	}
	var m mercuryStatus
	if err := json.Unmarshal(raw, &m); err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "mercury's status: "+err.Error(), nil)
		return
	}
	api.JSON(w, http.StatusOK, statusOf(m))
}
