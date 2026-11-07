// Package queue is the store-and-forward queue over the transport layer
// (docs/api-design/store-and-forward.md): the jobs waiting on UUCP and NNCP,
// cancelling one, calling a station now and stopping a call.
package queue

import (
	"errors"
	"log/slog"
	"net/http"
	"time"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/logging"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/transport"
)

// NewRunner runs the transports' programs, a variable for the tests.
var NewRunner = func() transport.Runner { return transport.ExecRunner{} }

type handlers struct {
	r   rad.Radio
	log *slog.Logger
}

func Register(rt *api.Router) {
	h := handlers{r: rt.Radio(), log: logging.New(rt.Radio(), "api.queue")}
	rt.Op("listQueue", h.list)
	rt.Op("cancelJob", h.cancel)
	rt.Op("callNow", h.call)
	rt.Op("stopCalls", h.stop)
}

// layer is the transport layer with the station's current settings.
func (h handlers) layer() *transport.Layer {
	t := h.r.Transport()
	return transport.New(transport.Settings{
		Prefer:           transport.Kind(t.Prefer),
		UucicoAirOptions: t.UucicoAirOptions,
		UucicoTCPSystems: t.UucicoTCPSystems,
	}, NewRunner())
}

// Job is QueueJob.
type Job struct {
	JobID     string   `json:"job_id"`
	Transport string   `json:"transport"`
	Station   string   `json:"station"`
	Kind      string   `json:"kind"`
	SizeBytes int64    `json:"size_bytes"`
	QueuedAt  *string  `json:"queued_at"`
	MailTo    []string `json:"mail_to"`
	MessageID *int64   `json:"message_id"`
}

func jobOf(j transport.Job) Job {
	out := Job{JobID: j.ID, Transport: string(j.Transport), Station: j.Station, Kind: j.Kind,
		SizeBytes: j.Bytes, MailTo: j.MailTo}
	if !j.QueuedAt.IsZero() {
		t := j.QueuedAt.UTC().Format(time.RFC3339)
		out.QueuedAt = &t
	}
	return out
}

func (h handlers) list(w http.ResponseWriter, r *http.Request) {
	station := r.URL.Query().Get("station")
	if station != "" && !transport.ValidName(station) {
		api.Invalid(w, r, api.Fields{"station": {"a station's UUCP name"}})
		return
	}
	jobs, err := h.layer().Queue(r.Context())
	if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", err.Error(), nil)
		return
	}
	out := []Job{}
	for _, j := range jobs {
		if station == "" || j.Station == station {
			out = append(out, jobOf(j))
		}
	}
	api.JSON(w, http.StatusOK, map[string]any{"data": out})
}

func (h handlers) cancel(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("job_id")
	err := h.layer().Cancel(r.Context(), id)
	if errors.Is(err, transport.ErrNotFound) {
		api.Problem(w, r, http.StatusNotFound, "not_found", "no such job", nil)
		return
	}
	if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", err.Error(), nil)
		return
	}
	p, _ := api.PrincipalFrom(r)
	h.log.Info("job cancelled", "user", p.UserID, "job", id)
	w.WriteHeader(http.StatusNoContent)
}

func (h handlers) call(w http.ResponseWriter, r *http.Request) {
	req := struct {
		Station   string `json:"station"`
		Transport string `json:"transport"`
	}{}
	if r.ContentLength != 0 && !api.Decode(w, r, &req) {
		return
	}
	via := transport.Kind(req.Transport)
	switch via {
	case "", transport.UUCP, transport.NNCP:
	case "reticulum":
		api.Problem(w, r, http.StatusUnprocessableEntity, "not_supported",
			"calls over Reticulum are not implemented yet", nil)
		return
	default:
		api.Invalid(w, r, api.Fields{"transport": {"uucp, nncp or reticulum"}})
		return
	}
	if req.Station != "" && !transport.ValidName(req.Station) {
		api.Invalid(w, r, api.Fields{"station": {"a station's UUCP name"}})
		return
	}
	called, err := h.layer().Call(req.Station, via)
	switch {
	case errors.Is(err, transport.ErrBusy):
		api.Problem(w, r, http.StatusConflict, "busy", err.Error(), nil)
	case errors.Is(err, transport.ErrUnknownStation):
		api.Problem(w, r, http.StatusNotFound, "not_found", err.Error(), nil)
	case errors.Is(err, transport.ErrNotSupported):
		api.Problem(w, r, http.StatusUnprocessableEntity, "not_supported",
			"that transport does not reach the station", nil)
	case err != nil:
		api.Problem(w, r, http.StatusInternalServerError, "internal", err.Error(), nil)
	default:
		p, _ := api.PrincipalFrom(r)
		h.log.Info("radio.tx call", "user", p.UserID, "station", req.Station, "transports", called)
		var station *string
		if req.Station != "" {
			station = &req.Station
		}
		api.JSON(w, http.StatusAccepted, map[string]any{"station": station, "transports": called})
	}
}

func (h handlers) stop(w http.ResponseWriter, r *http.Request) {
	n := h.layer().Stop()
	p, _ := api.PrincipalFrom(r)
	h.log.Info("calls stopped", "user", p.UserID, "processes", n)
	w.WriteHeader(http.StatusNoContent)
}
