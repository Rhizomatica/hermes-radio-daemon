// Package broadcasts serves the files on Mercury's broadcast plane
// (api-rest.yaml, tag broadcasts): the queue going out as a carousel and the
// files received.
//
// radiod's C side is Mercury's broadcast client (modem/bcast_plane.h). It
// sends the oldest file in the spool's tx directory and files what it
// receives in rx; NNCP uses the same directories. So the queue is the tx
// directory, and this package works on the files: it queues an upload by
// writing it there (whole, by a rename), takes one out by removing it, and
// lists and serves rx.
package broadcasts

import (
	"errors"
	"io"
	"log/slog"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/logging"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/ratelimit"
)

const (
	// MaxBytes is the largest file a carousel carries: Mercury's object
	// limit, 1440 KiB with its name (BCAST_FILE_MAX_BYTES).
	MaxBytes = 1440 * 1024
	// PassesDefault and PassesMax are createBroadcast's passes.
	PassesDefault = 2
	PassesMax     = 16
	// PerHour is how many broadcasts one user may queue an hour
	// (settings.tx_rate_limit.broadcasts_per_hour, its default).
	PerHour = 6
)

// nameRe is BroadcastName.
var nameRe = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,254}$`)

type handlers struct {
	r     rad.Radio
	limit *ratelimit.Limiter
	log   *slog.Logger
}

// Register adds the broadcast operations.
func Register(rt *api.Router) {
	h := handlers{r: rt.Radio(), limit: &ratelimit.Limiter{}, log: logging.New(rt.Radio(), "api.tx")}
	rt.Op("listBroadcasts", h.list)
	rt.Op("createBroadcast", h.create)
	rt.Op("getBroadcast", h.get)
	rt.Op("cancelBroadcast", h.cancel)
	rt.Op("listReceivedBroadcasts", h.listReceived)
	rt.Op("downloadReceivedBroadcast", h.download)
	rt.Op("deleteReceivedBroadcast", h.deleteReceived)
}

// ── response schemas (api-rest.yaml components) ──

// File is BroadcastFile.
type File struct {
	ID            string `json:"id"`
	Name          string `json:"name"`
	Bytes         int64  `json:"bytes"`
	QueuedAt      string `json:"queued_at"`
	State         string `json:"state"`
	FramesLimit   *int64 `json:"frames_limit"`
	FramesSent    uint64 `json:"frames_sent"`
	FramesPerPass int64  `json:"frames_per_pass"`
	Pass          *int   `json:"pass"`
	Passes        *int   `json:"passes"`
}

// Received is ReceivedBroadcast.
type Received struct {
	Name       string `json:"name"`
	Bytes      int64  `json:"bytes"`
	ReceivedAt string `json:"received_at"`
}

// plane is the broadcast plane, or a 503 when it is not running.
func (h handlers) plane(w http.ResponseWriter, r *http.Request) (rad.Broadcast, bool) {
	b, err := h.r.Broadcast()
	if err != nil || !b.Running {
		api.Problem(w, r, http.StatusServiceUnavailable, "not_running",
			"the broadcast plane is not running (main:broadcast_enable)", nil)
		return b, false
	}
	return b, true
}

func iso(t time.Time) string { return t.UTC().Format(time.RFC3339) }

// files are the regular, not hidden files of dir, oldest first.
func files(dir string) []os.FileInfo {
	ents, _ := os.ReadDir(dir)
	var out []os.FileInfo
	for _, e := range ents {
		if strings.HasPrefix(e.Name(), ".") || !e.Type().IsRegular() {
			continue
		}
		if fi, err := e.Info(); err == nil {
			out = append(out, fi)
		}
	}
	sort.Slice(out, func(i, j int) bool {
		if !out[i].ModTime().Equal(out[j].ModTime()) {
			return out[i].ModTime().Before(out[j].ModTime())
		}
		return out[i].Name() < out[j].Name()
	})
	return out
}

func (h handlers) fileOf(b rad.Broadcast, fi os.FileInfo) File {
	name, limit := h.r.BroadcastFileInfo(fi.Name())
	f := File{
		ID: fi.Name(), Name: name, Bytes: fi.Size(), QueuedAt: iso(fi.ModTime()), State: "waiting",
		FramesPerPass: h.r.BroadcastFramesPerPass(name, uint64(fi.Size())),
	}
	if limit > 0 {
		f.FramesLimit = &limit
		if f.FramesPerPass > 0 {
			p := int((limit + f.FramesPerPass - 1) / f.FramesPerPass)
			f.Passes = &p
		}
	}
	if b.Path != "" && filepath.Base(b.Path) == fi.Name() && b.State == rad.BroadcastSending {
		f.State = "sending"
		f.FramesSent = b.FramesSent
		pass := b.Pass
		f.Pass = &pass
	}
	return f
}

// queue is the tx directory as the queue: the file on the air first.
func (h handlers) queue(b rad.Broadcast) []File {
	out := []File{}
	for _, fi := range files(b.TxDir) {
		f := h.fileOf(b, fi)
		if f.State == "sending" {
			out = append([]File{f}, out...)
		} else {
			out = append(out, f)
		}
	}
	return out
}

// ── the queue ──

func (h handlers) list(w http.ResponseWriter, r *http.Request) {
	b, ok := h.plane(w, r)
	if !ok {
		return
	}
	var mode *string
	if b.ModeName != "" {
		mode = &b.ModeName
	}
	api.JSON(w, http.StatusOK, map[string]any{
		"running": b.Running, "connected": b.Connected, "mode": mode, "data": h.queue(b),
	})
}

func (h handlers) get(w http.ResponseWriter, r *http.Request) {
	b, ok := h.plane(w, r)
	if !ok {
		return
	}
	for _, f := range h.queue(b) {
		if f.ID == r.PathValue("broadcast_id") {
			api.JSON(w, http.StatusOK, f)
			return
		}
	}
	api.Problem(w, r, http.StatusNotFound, "not_found", "no such broadcast in the queue", nil)
}

func (h handlers) create(w http.ResponseWriter, r *http.Request) {
	b, ok := h.plane(w, r)
	if !ok {
		return
	}
	q := r.URL.Query()
	f := api.Fields{}
	name := q.Get("name")
	if !nameRe.MatchString(name) {
		f.Add("name", "a file name: letters, digits, . _ -, starting with a letter or digit")
	} else if _, limit := h.r.BroadcastFileInfo(name); limit > 0 {
		f.Add("name", "without a -<N>_frames budget: passes sets how long it goes out")
	}
	passes := PassesDefault
	if s := q.Get("passes"); s != "" {
		n, err := strconv.Atoi(s)
		if err != nil || n < 1 || n > PassesMax {
			f.Add("passes", "1 to "+strconv.Itoa(PassesMax))
		}
		passes = n
	}
	if len(f) > 0 {
		api.Invalid(w, r, f)
		return
	}
	data, err := io.ReadAll(io.LimitReader(r.Body, MaxBytes+1))
	if err != nil {
		api.Problem(w, r, http.StatusBadRequest, "bad_request", "cannot read the body", nil)
		return
	}
	if len(data) > MaxBytes {
		api.Problem(w, r, http.StatusRequestEntityTooLarge, "too_large",
			"a broadcast carries at most "+strconv.Itoa(MaxBytes)+" bytes", nil)
		return
	}
	if len(data) == 0 {
		api.Invalid(w, r, api.Fields{"body": {"the file, not empty"}})
		return
	}
	perPass := h.r.BroadcastFramesPerPass(name, uint64(len(data)))
	if perPass <= 0 {
		api.Problem(w, r, http.StatusConflict, "listen_mode_unsuitable",
			"the listen mode "+b.ModeName+" cannot carry files", nil)
		return
	}
	for _, q := range h.queue(b) {
		if q.Name == name {
			api.Problem(w, r, http.StatusConflict, "conflict", "a broadcast named "+name+" is queued already", nil)
			return
		}
	}
	p, _ := api.PrincipalFrom(r)
	if ok, retry, _ := h.limit.Reserve("broadcast:"+strconv.FormatInt(p.UserID, 10), PerHour, time.Hour); !ok {
		w.Header().Set("Retry-After", strconv.Itoa(int(retry.Seconds())+1))
		api.Problem(w, r, http.StatusTooManyRequests, "rate_limited",
			"queued "+strconv.Itoa(PerHour)+" broadcasts this hour", nil)
		return
	}

	// "<stem>-<frames>_frames<ext>": the spool's budget, so it leaves the
	// queue after its passes.
	ext := filepath.Ext(name)
	id := strings.TrimSuffix(name, ext) + "-" + strconv.FormatInt(int64(passes)*perPass, 10) + "_frames" + ext
	tmp, err := os.CreateTemp(b.TxDir, ".upload-*")
	if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "cannot write to the broadcast spool", nil)
		return
	}
	_, werr := tmp.Write(data)
	cerr := tmp.Close()
	if werr != nil || cerr != nil || os.Rename(tmp.Name(), filepath.Join(b.TxDir, id)) != nil {
		os.Remove(tmp.Name())
		api.Problem(w, r, http.StatusInternalServerError, "internal", "cannot write to the broadcast spool", nil)
		return
	}
	h.log.Info("radio.tx broadcast", "user", p.UserID, "name", name, "bytes", len(data),
		"passes", passes, "frames", int64(passes)*perPass, "mode", b.ModeName)
	fi, err := os.Stat(filepath.Join(b.TxDir, id))
	if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "the queued file is gone", nil)
		return
	}
	api.JSON(w, http.StatusCreated, h.fileOf(b, fi))
}

func (h handlers) cancel(w http.ResponseWriter, r *http.Request) {
	b, ok := h.plane(w, r)
	if !ok {
		return
	}
	id := r.PathValue("broadcast_id")
	if !nameRe.MatchString(id) {
		api.Problem(w, r, http.StatusNotFound, "not_found", "no such broadcast in the queue", nil)
		return
	}
	// The spool stops the one on the air when its file goes.
	if err := os.Remove(filepath.Join(b.TxDir, id)); err != nil {
		if errors.Is(err, os.ErrNotExist) {
			api.Problem(w, r, http.StatusNotFound, "not_found", "no such broadcast in the queue", nil)
		} else {
			api.Problem(w, r, http.StatusInternalServerError, "internal", "cannot remove it from the spool", nil)
		}
		return
	}
	p, _ := api.PrincipalFrom(r)
	h.log.Info("broadcast cancelled", "user", p.UserID, "id", id)
	w.WriteHeader(http.StatusNoContent)
}

// ── received ──

func (h handlers) listReceived(w http.ResponseWriter, r *http.Request) {
	b, ok := h.plane(w, r)
	if !ok {
		return
	}
	fis := files(b.RxDir)
	out := make([]Received, 0, len(fis))
	for i := len(fis) - 1; i >= 0; i-- { // newest first
		out = append(out, Received{Name: fis[i].Name(), Bytes: fis[i].Size(), ReceivedAt: iso(fis[i].ModTime())})
	}
	api.JSON(w, http.StatusOK, map[string]any{"data": out})
}

// received is the path of a file in rx, or "" (a 404 sent) when there is none.
func (h handlers) received(w http.ResponseWriter, r *http.Request) (string, bool) {
	b, ok := h.plane(w, r)
	if !ok {
		return "", false
	}
	name := r.PathValue("name")
	path := filepath.Join(b.RxDir, name)
	if fi, err := os.Stat(path); !nameRe.MatchString(name) || err != nil || !fi.Mode().IsRegular() {
		api.Problem(w, r, http.StatusNotFound, "not_found", "no such file received", nil)
		return "", false
	}
	return path, true
}

func (h handlers) download(w http.ResponseWriter, r *http.Request) {
	path, ok := h.received(w, r)
	if !ok {
		return
	}
	data, err := os.ReadFile(path)
	if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "cannot read the file", nil)
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Disposition", `attachment; filename="`+filepath.Base(path)+`"`)
	w.WriteHeader(http.StatusOK)
	w.Write(data)
}

func (h handlers) deleteReceived(w http.ResponseWriter, r *http.Request) {
	path, ok := h.received(w, r)
	if !ok {
		return
	}
	if err := os.Remove(path); err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "cannot delete the file", nil)
		return
	}
	w.WriteHeader(http.StatusNoContent)
}
