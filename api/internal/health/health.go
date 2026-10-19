// Package health serves getHealth: liveness, and the state of what this
// daemon runs (the radio; mercury when it is linked in).
package health

import (
	"net/http"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
)

// Register adds getHealth.
func Register(rt *api.Router) {
	r := rt.Radio()
	rt.Op("getHealth", func(w http.ResponseWriter, req *http.Request) {
		comp := map[string]string{"radio": "ok"}
		if r.ProfileCount() == 0 {
			comp["radio"] = "down"
		}
		if _, ok := r.ModemStatus(); ok {
			comp["modem"] = "ok"
		}
		status := "ok"
		if comp["radio"] != "ok" {
			status = "degraded"
		}
		api.JSON(w, http.StatusOK, map[string]any{"status": status, "components": comp})
	})
}
