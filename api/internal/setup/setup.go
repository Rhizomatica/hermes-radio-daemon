// Package setup serves the first-boot setup (api-rest.yaml, tag setup):
// report whether it is done, and run it once — against the installer's
// one-time code — to create the first admin and record the station identity.
package setup

import (
	"context"
	"crypto/subtle"
	"errors"
	"log/slog"
	"net/http"
	"os"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/auth"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/config"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/passwd"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/ratelimit"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/users"
)

// Config tunes Register.
type Config struct {
	// SetupCodePath is where the installer's one-time code lives; "" means
	// /etc/hermes/setup-code.
	SetupCodePath string
	// SessionLifetime is how long the setup-issued admin session lasts.
	SessionLifetime time.Duration
}

type handlers struct {
	store    *store.Store
	codePath string
	lifetime time.Duration
	limiter  *ratelimit.Limiter // wrong setup codes: 5 per client, then locked 10 minutes
	global   *ratelimit.Limiter // and MaxWrongCodesPerHour per hour from all clients together
}

// Register adds the setup operations.
func Register(rt *api.Router, st *store.Store, cfg Config) {
	if cfg.SetupCodePath == "" {
		cfg.SetupCodePath = config.DefaultSetupCode
	}
	if cfg.SessionLifetime <= 0 {
		cfg.SessionLifetime = auth.DefaultLifetime
	}
	h := &handlers{
		store: st, codePath: cfg.SetupCodePath,
		lifetime: cfg.SessionLifetime, limiter: &ratelimit.Limiter{}, global: &ratelimit.Limiter{},
	}
	rt.StationCallsign = func(ctx context.Context) string {
		v, ok, err := st.GetMeta(ctx, "station.callsign")
		if err != nil || !ok {
			return ""
		}
		return v
	}
	rt.Op("getSetupStatus", h.status)
	rt.Op("runSetup", h.run)
}

var (
	usernameRe = regexp.MustCompile(`^[a-z0-9._-]{1,32}$`)
	countryRe  = regexp.MustCompile(`^[A-Z]{2}$`)
	locales    = map[string]bool{"en": true, "es": true, "pt-BR": true, "fr": true}
)

// MaxWrongCodesPerHour is how many wrong setup codes the station takes an hour from
// all clients together, however many addresses they come from.
const MaxWrongCodesPerHour = 30

// done reports whether first-boot setup has completed.
func (h *handlers) done(r *http.Request) (bool, error) {
	v, ok, err := h.store.GetMeta(r.Context(), "setup.done")
	if err != nil {
		return false, err
	}
	return ok && v == "1", nil
}

func (h *handlers) status(w http.ResponseWriter, r *http.Request) {
	done, err := h.done(r)
	if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
		return
	}
	api.JSON(w, http.StatusOK, map[string]any{"done": done})
}

func (h *handlers) run(w http.ResponseWriter, r *http.Request) {
	done, err := h.done(r)
	if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
		return
	}
	if done {
		api.Problem(w, r, http.StatusConflict, "setup_done", "setup was already completed", nil)
		return
	}

	var req struct {
		SetupCode   string  `json:"setup_code"`
		StationName string  `json:"station_name"`
		Callsign    *string `json:"callsign"`
		Country     *string `json:"country"`
		Language    string  `json:"language"`
		Timezone    string  `json:"timezone"`
		Admin       struct {
			Username    string  `json:"username"`
			Password    string  `json:"password"`
			DisplayName *string `json:"display_name"`
		} `json:"admin"`
	}
	if !api.Decode(w, r, &req) {
		return
	}
	f := api.Fields{}
	if req.SetupCode == "" {
		f.Add("setup_code", "required")
	}
	if req.StationName == "" || len(req.StationName) > 64 {
		f.Add("station_name", "1 to 64 characters")
	}
	if req.Callsign != nil && len(*req.Callsign) > 16 {
		f.Add("callsign", "at most 16 characters")
	}
	if req.Country != nil && !countryRe.MatchString(*req.Country) {
		f.Add("country", "ISO 3166-1 alpha-2, e.g. BR")
	}
	if !locales[req.Language] {
		f.Add("language", "en, es, pt-BR or fr")
	}
	if req.Timezone == "" {
		f.Add("timezone", "required, e.g. America/Sao_Paulo")
	}
	if !usernameRe.MatchString(req.Admin.Username) {
		f.Add("admin.username", "1 to 32 characters: a-z, 0-9, . _ -")
	}
	if len(req.Admin.Password) < 8 {
		f.Add("admin.password", "at least 8 characters")
	}
	if req.Admin.DisplayName != nil && len(*req.Admin.DisplayName) > 64 {
		f.Add("admin.display_name", "at most 64 characters")
	}
	if !f.OK() {
		api.Invalid(w, r, f)
		return
	}

	// The attempt is counted before the code is read, so parallel guesses
	// cannot all pass the check first; a right code (or a server fault)
	// gives it back.
	// Two buckets: the client's, and one for the whole station, so guessers
	// spread over many addresses still get only MaxWrongCodesPerHour tries an hour
	// at the one-time code.
	key := "setup:" + auth.RateKey(r)
	ok, retry, mine := h.limiter.Reserve(key, 5, 10*time.Minute)
	if !ok {
		w.Header().Set("Retry-After", strconv.Itoa(int(retry.Seconds())+1))
		api.Problem(w, r, http.StatusTooManyRequests, "rate_limited", "too many wrong setup codes", nil)
		return
	}
	ok, retry, all := h.global.Reserve("setup", MaxWrongCodesPerHour, time.Hour)
	if !ok {
		h.limiter.Release(mine)
		w.Header().Set("Retry-After", strconv.Itoa(int(retry.Seconds())+1))
		api.Problem(w, r, http.StatusTooManyRequests, "rate_limited", "too many wrong setup codes", nil)
		return
	}
	release := func() { h.limiter.Release(mine); h.global.Release(all) }
	code, err := os.ReadFile(h.codePath)
	if err != nil {
		release()
		if errors.Is(err, os.ErrNotExist) {
			alreadyDone, derr := h.done(r)
			if derr != nil {
				api.Problem(w, r, http.StatusInternalServerError, "internal", "the database failed", nil)
				return
			}
			if alreadyDone {
				// A concurrent setup finished and retired the code.
				api.Problem(w, r, http.StatusConflict, "setup_done", "setup was already completed", nil)
				return
			}
		}
		api.Problem(w, r, http.StatusInternalServerError, "internal", "no setup code available", nil)
		return
	}
	expected := strings.TrimSpace(string(code))
	if subtle.ConstantTimeCompare([]byte(req.SetupCode), []byte(expected)) != 1 {
		api.Problem(w, r, http.StatusForbidden, "bad_setup_code", "wrong setup code", nil)
		return
	}
	release()

	hash, err := passwd.Hash(req.Admin.Password)
	if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "cannot hash the password", nil)
		return
	}
	metas := map[string]string{
		"station.name":     req.StationName,
		"station.language": req.Language,
		"station.timezone": req.Timezone,
		"setup.done":       "1",
	}
	if req.Callsign != nil {
		metas["station.callsign"] = *req.Callsign
	}
	if req.Country != nil {
		metas["station.country"] = *req.Country
	}
	admin, err := h.store.CompleteSetup(r.Context(), store.NewUser{
		Username: req.Admin.Username, PasswordHash: hash, Role: "admin",
		DisplayName: req.Admin.DisplayName,
	}, metas)
	switch {
	case errors.Is(err, store.ErrSetupDone):
		// Lost a race with a concurrent setup.
		api.Problem(w, r, http.StatusConflict, "setup_done", "setup was already completed", nil)
		return
	case errors.Is(err, store.ErrUsernameTaken):
		api.Problem(w, r, http.StatusConflict, "conflict", "the admin username is taken", nil)
		return
	case err != nil:
		api.Problem(w, r, http.StatusInternalServerError, "internal", "cannot complete setup", nil)
		return
	}
	// The code is one-time: retire it now that it has been used.
	if err := os.Remove(h.codePath); err != nil && !errors.Is(err, os.ErrNotExist) {
		slog.Warn("cannot remove the used setup code", "path", h.codePath, "error", err)
	}

	token, expires, err := auth.IssueSession(w, r, h.store, h.lifetime, admin)
	if err != nil {
		api.Problem(w, r, http.StatusInternalServerError, "internal", "cannot make a session token", nil)
		return
	}
	api.JSON(w, http.StatusCreated, auth.Session{User: users.Of(admin), Token: token, ExpiresAt: expires})
}
