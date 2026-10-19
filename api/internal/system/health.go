// Package system groups process-level concerns: health, observability, and
// (later) reboot. Handlers here report on hermesd itself rather than on a REST
// domain.
package system

import (
	"context"
	"encoding/json"
	"log/slog"
	"net/http"
	"time"
)

// Pinger is the slice of the storage layer the health check needs.
// *store.Store satisfies it.
type Pinger interface {
	PingContext(ctx context.Context) error
}

// Health returns a handler for GET /health. It reports process liveness and,
// when db is non-nil, database reachability.
func Health(logger *slog.Logger, db Pinger) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		body := map[string]any{
			"status":  "ok",
			"version": "0.1.0",
		}
		status := http.StatusOK

		if db != nil {
			ctx, cancel := context.WithTimeout(r.Context(), 2*time.Second)
			defer cancel()
			if err := db.PingContext(ctx); err != nil {
				logger.Error("health: database unreachable", "error", err)
				body["status"] = "degraded"
				body["database"] = "unavailable"
				status = http.StatusServiceUnavailable
			} else {
				body["database"] = "ok"
			}
		}

		writeJSON(w, status, body)
	}
}

func writeJSON(w http.ResponseWriter, status int, body any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(body)
}
