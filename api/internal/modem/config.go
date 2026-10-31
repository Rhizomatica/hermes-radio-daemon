package modem

import (
	"encoding/json/v2"
	"errors"
	"fmt"
	"net/http"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
)

// Config is ModemConfig: what radiod still sets in the mercury it runs.
// tx_gain_db and waterfall are mercury's own (its mercury.ini), the listen
// mode radiod's (user.ini), as mercury keeps none across restarts.
type Config struct {
	TxGainDB   float64 `json:"tx_gain_db"`
	Waterfall  bool    `json:"waterfall"`
	ListenMode *string `json:"listen_mode"`
}

type configPatch struct {
	TxGainDB   *float64 `json:"tx_gain_db"`
	Waterfall  *bool    `json:"waterfall"`
	ListenMode *string  `json:"listen_mode"`
}

// config reads mercury's configuration: the gain and spectrum feed from
// its last status, the listen mode from mercury itself.
func (h handlers) config() (Config, error) {
	raw, ok := h.r.ModemStatus()
	if !ok {
		return Config{}, errNotRunning
	}
	var m mercuryStatus
	if err := json.Unmarshal(raw, &m); err != nil {
		return Config{}, fmt.Errorf("mercury's status: %w", err)
	}
	c := Config{TxGainDB: m.TxGainDB, Waterfall: m.Waterfall, ListenMode: listenMode(m.ListenMode)}
	if name, err := h.r.ListenMode(); err == nil {
		c.ListenMode = listenMode(name)
	}
	return c, nil
}

func (h handlers) getConfig(w http.ResponseWriter, r *http.Request) {
	c, err := h.config()
	switch {
	case errors.Is(err, errNotRunning):
		api.Problem(w, r, http.StatusServiceUnavailable, "not_running", err.Error(), nil)
	case err != nil:
		api.Problem(w, r, http.StatusInternalServerError, "internal", err.Error(), nil)
	default:
		api.JSON(w, http.StatusOK, c)
	}
}

// updateConfig checks every field before it applies any. The listen mode
// goes first, as the one change mercury can refuse (busy): then nothing
// was applied.
func (h handlers) updateConfig(w http.ResponseWriter, r *http.Request) {
	var p configPatch
	if !api.Decode(w, r, &p) {
		return
	}
	f := api.Fields{}
	if p.TxGainDB != nil && (*p.TxGainDB < -20 || *p.TxGainDB > 20) {
		f.Add("tx_gain_db", "from -20 to 20")
	}
	if p.ListenMode != nil && !listenModes[*p.ListenMode] {
		f.Add("listen_mode", "one of DATAC1, DATAC3, DATAC4, DATAC15, DATAC17, QAM16C2, MFSK, MFSK500")
	}
	if !f.OK() {
		api.Invalid(w, r, f)
		return
	}
	c, err := h.config()
	if err != nil {
		api.Problem(w, r, http.StatusServiceUnavailable, "not_running", err.Error(), nil)
		return
	}
	if p.ListenMode != nil {
		if err := h.r.SetListenMode(*p.ListenMode); err != nil {
			h.configErr(w, r, "listen_mode", err)
			return
		}
		c.ListenMode = listenMode(*p.ListenMode)
	}
	if p.TxGainDB != nil {
		if err := h.r.SetModemTxGain(*p.TxGainDB); err != nil {
			h.configErr(w, r, "tx_gain_db", err)
			return
		}
		c.TxGainDB = *p.TxGainDB
	}
	if p.Waterfall != nil {
		if err := h.r.SetModemWaterfall(*p.Waterfall); err != nil {
			h.configErr(w, r, "waterfall", err)
			return
		}
		c.Waterfall = *p.Waterfall
	}
	// mercury's status says the new values within a second; until then, the
	// reply has them from the patch
	api.JSON(w, http.StatusOK, c)
}

func (h handlers) configErr(w http.ResponseWriter, r *http.Request, field string, err error) {
	switch {
	case errors.Is(err, rad.ErrBusy):
		why, _ := h.r.ModemBusy()
		detail := "the modem is busy"
		switch why {
		case "arq":
			detail = "an ARQ link is up"
		case "broadcast":
			detail = "a broadcast is on the air"
		}
		api.Problem(w, r, http.StatusConflict, "busy", detail, nil)
	case errors.Is(err, rad.ErrNotSupported):
		api.Problem(w, r, http.StatusServiceUnavailable, "not_running", err.Error(), nil)
	default:
		api.Invalid(w, r, api.Fields{field: {err.Error()}})
	}
}
