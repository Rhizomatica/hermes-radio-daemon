package gps

import (
	"context"
	"path/filepath"
	"sync"
	"testing"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/logging"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
)

// TestRecordWritesPointIntoOpenTrip exercises record() directly (no ticker),
// so the recorder's behaviour is deterministic: it writes one point per fix
// into the open trip, and nothing while no trip is open or there is no fix.
func TestRecordWritesPointIntoOpenTrip(t *testing.T) {
	st, err := store.Open(context.Background(), filepath.Join(t.TempDir(), "hermes.sqlite"))
	if err != nil {
		t.Fatal(err)
	}
	defer st.Close()

	s := rad.NewStub()
	h := handlers{r: s, st: st, mu: &sync.Mutex{}, log: logging.New(s, "api.tx")}

	// No open trip: a no-op.
	h.record()

	trip, err := st.OpenTrip(context.Background())
	if err != nil {
		t.Fatal(err)
	}

	// No fix yet: still a no-op.
	h.record()
	points, _, err := st.TripPoints(context.Background(), trip.ID, 50, 0)
	if err != nil {
		t.Fatal(err)
	}
	if len(points) != 0 {
		t.Fatalf("points with no fix = %d, want 0", len(points))
	}

	lat, lon, alt := 19.43, -99.13, 2240.0
	at := "2026-11-09T12:00:00Z"
	s.SetGpsFix("3d", &lat, &lon, &alt, &at)
	h.record()

	points, _, err = st.TripPoints(context.Background(), trip.ID, 50, 0)
	if err != nil {
		t.Fatal(err)
	}
	if len(points) != 1 {
		t.Fatalf("points after a fix = %d, want 1", len(points))
	}
	p := points[0]
	if p.At != at || p.Latitude != lat || p.Longitude != lon ||
		p.AltitudeM == nil || *p.AltitudeM != alt || p.AccuracyM != nil {
		t.Fatalf("point = %+v", p)
	}
}
