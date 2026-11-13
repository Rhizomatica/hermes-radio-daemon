package gps_test

import (
	"bytes"
	"encoding/base64"
	"errors"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"testing"
	"time"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/gps"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/testapi"
)

// t0 is 2026-11-09T00:00:00Z.
var t0 = time.Date(2026, 11, 9, 0, 0, 0, 0, time.UTC)

// minutes is a point a minute from t0, moving north.
func minutes(n int) []rad.GpsPoint {
	pts := make([]rad.GpsPoint, n)
	for i := range pts {
		pts[i] = rad.GpsPoint{
			AtMs: t0.Add(time.Duration(i) * time.Minute).UnixMilli(), LatE7: 387000000 + int32(i)*1000,
			LonE7: -91000000, AltDm: rad.GpsNoAlt, AccuracyDm: rad.GpsNoAcc, Fix: rad.GpsFix2D,
		}
	}
	pts[0].AltDm, pts[0].AccuracyDm, pts[0].Fix = 513, 35, rad.GpsFix3D
	return pts
}

func TestTrackRangeAndPages(t *testing.T) {
	e := testapi.New(t)
	e.Radio.SetGpsTrack("", minutes(10)...)
	q := "/gps/track?since=2026-11-09T00:00:00Z&until=2026-11-09T00:06:00Z&limit=3"
	j := e.Do("GET", q, "").JSON(t)
	data := j["data"].([]any)
	if len(data) != 3 {
		t.Fatalf("first page = %v", j)
	}
	p := data[0].(map[string]any)
	if p["at"] != "2026-11-09T00:00:00.000Z" || p["latitude"] != 38.7 || p["longitude"] != -9.1 ||
		p["altitude_m"] != 51.3 || p["accuracy_m"] != 3.5 || p["fix"] != "3d" || p["emergency"] != 0.0 {
		t.Errorf("first point = %v", p)
	}
	if p = data[1].(map[string]any); p["altitude_m"] != nil || p["accuracy_m"] != nil || p["fix"] != "2d" {
		t.Errorf("second point = %v", p)
	}
	// pages go on until the range's end, 00:06 included: 7 points
	got := len(data)
	for next, _ := j["next_cursor"].(string); next != ""; next, _ = j["next_cursor"].(string) {
		j = e.Do("GET", q+"&cursor="+url.QueryEscape(next), "").JSON(t)
		got += len(j["data"].([]any))
	}
	if got != 7 {
		t.Errorf("points over the pages = %d, want 7", got)
	}
	if last := j["data"].([]any); last[len(last)-1].(map[string]any)["at"] != "2026-11-09T00:06:00.000Z" {
		t.Errorf("last page = %v", last)
	}
}

// Points that share a timestamp are a run: pages must neither skip nor
// repeat one when the run crosses a page boundary.
func TestTrackPagesSameTime(t *testing.T) {
	e := testapi.New(t)
	at := t0.UnixMilli()
	pts := make([]rad.GpsPoint, 5)
	for i := range pts {
		pts[i] = rad.GpsPoint{AtMs: at, LatE7: int32(387000000 + i)}
	}
	e.Radio.SetGpsTrack("", pts...)

	q := "/gps/track?since=2026-11-09T00:00:00Z&until=2026-11-09T00:00:00Z&limit=2"
	var got []float64
	j := e.Do("GET", q, "").JSON(t)
	for {
		for _, p := range j["data"].([]any) {
			got = append(got, p.(map[string]any)["latitude"].(float64))
		}
		next, _ := j["next_cursor"].(string)
		if next == "" {
			break
		}
		j = e.Do("GET", q+"&cursor="+url.QueryEscape(next), "").JSON(t)
	}
	if len(got) != len(pts) {
		t.Fatalf("points over the pages = %v, want %d", got, len(pts))
	}
	for i, lat := range got {
		if want := float64(387000000+i) / 1e7; lat != want {
			t.Errorf("point %d = %v, want %v", i, lat, want)
		}
	}
}

func TestTrackDefaultsAndCallsign(t *testing.T) {
	e := testapi.New(t)
	now := time.Now()
	e.Radio.SetGpsTrack("",
		rad.GpsPoint{AtMs: now.Add(-25 * time.Hour).UnixMilli()},
		rad.GpsPoint{AtMs: now.Add(-time.Hour).UnixMilli()})
	if data := e.Do("GET", "/gps/track", "").JSON(t)["data"].([]any); len(data) != 1 {
		t.Errorf("the last day = %v", data)
	}
	e.Radio.SetGpsTrack("B0AB/P", rad.GpsPoint{AtMs: now.Add(-time.Minute).UnixMilli(), Emergency: 8,
		Fix: rad.GpsFixManual})
	data := e.Do("GET", "/gps/track?callsign=b0ab/p", "").JSON(t)["data"].([]any)
	if len(data) != 1 || data[0].(map[string]any)["emergency"] != 8.0 || data[0].(map[string]any)["fix"] != "manual" {
		t.Errorf("B0AB/P = %v", data)
	}
	n, err := gps.Count(e.Radio, "B0AB/P", now.Add(-time.Hour), now)
	if err != nil || n != 1 {
		t.Errorf("Count = %d %v", n, err)
	}
}

func TestTrackInvalid(t *testing.T) {
	e := testapi.New(t)
	for _, q := range []string{
		"since=yesterday", "until=2026-11-09T00:00:00Z&since=2026-11-09T01:00:00Z", "limit=0",
		"limit=5001", "callsign=B0AB_P", "callsign=ABCDEFGHIJKLMNOP", "cursor=!!",
	} {
		if r := e.Do("GET", "/gps/track?"+q, ""); r.Status != 422 {
			t.Errorf("%s = %d %s", q, r.Status, r.Body)
		}
	}
}

func TestGpsFiles(t *testing.T) {
	e := testapi.New(t)
	dir := t.TempDir()
	a := filepath.Join(dir, "gps-20261109T000000Z.trk")
	b := filepath.Join(dir, "gps-20261109T010000Z.trk")
	os.WriteFile(a, []byte("HGPS\x01\x18\x00\x00"), 0o644)
	os.WriteFile(b, bytes.Repeat([]byte{1}, 32), 0o644)
	e.Radio.SetGpsTrackFiles(a, b)

	data := e.Do("GET", "/gps/files", "").JSON(t)["data"].([]any)
	if len(data) != 2 {
		t.Fatalf("files = %v", data)
	}
	f := data[1].(map[string]any)
	if f["name"] != "gps-20261109T010000Z.trk" || f["size_bytes"] != 32.0 || f["created_at"] != "2026-11-09T01:00:00Z" {
		t.Errorf("file = %v", f)
	}

	r := e.Do("GET", "/gps/files/gps-20261109T000000Z.trk", "")
	if r.Status != 200 || string(r.Body) != "HGPS\x01\x18\x00\x00" || r.Header.Get("Content-Type") != "application/octet-stream" {
		t.Errorf("download = %d %q %v", r.Status, r.Body, r.Header)
	}
	if r := e.Do("GET", "/gps/files/gps-20261108T000000Z.trk", ""); r.Status != 404 {
		t.Errorf("no such file = %d", r.Status)
	}

	if r := e.Do("DELETE", "/gps/files", ""); r.Status != 204 {
		t.Fatalf("DELETE = %d %s", r.Status, r.Body)
	}
	if _, err := os.Stat(a); !os.IsNotExist(err) {
		t.Errorf("%s still there", a)
	}
	if data := e.Do("GET", "/gps/files", "").JSON(t)["data"].([]any); len(data) != 0 {
		t.Errorf("after DELETE = %v", data)
	}
}

// Track with no room for a point returns none, and does not panic.
func TestTrackNoLimit(t *testing.T) {
	e := testapi.New(t)
	e.Radio.SetGpsTrack("", minutes(3)...)
	for _, limit := range []int{0, -1} {
		pts, next, err := gps.Track(e.Radio, "", t0, t0.Add(time.Hour), limit)
		if err != nil || len(pts) != 0 || !next.IsZero() {
			t.Errorf("limit %d = %v %v %v", limit, pts, next, err)
		}
	}
}

// A directory that cannot be read is a 500, not an empty list; a file that
// cannot be removed fails the delete; one gone already does not.
func TestGpsFilesFailures(t *testing.T) {
	e := testapi.New(t)
	e.Radio.SetGpsTrackFilesError(errors.New("the track's directory cannot be read"))
	for _, c := range []struct{ method, path string }{
		{"GET", "/gps/files"}, {"GET", "/gps/files/gps-20261109T000000Z.trk"}, {"DELETE", "/gps/files"},
	} {
		if r := e.Do(c.method, c.path, ""); r.Status != 500 {
			t.Errorf("%s %s = %d", c.method, c.path, r.Status)
		}
	}
	e.Radio.SetGpsTrackFilesError(nil)

	dir := t.TempDir()
	gone := filepath.Join(dir, "gps-20261109T000000Z.trk") // listed, never written: trimmed meanwhile
	e.Radio.SetGpsTrackFiles(gone)
	if r := e.Do("DELETE", "/gps/files", ""); r.Status != 204 {
		t.Errorf("DELETE with a file gone already = %d %s", r.Status, r.Body)
	}

	stuck := filepath.Join(dir, "sub", "gps-20261109T010000Z.trk")
	os.MkdirAll(filepath.Join(dir, "sub", "gps-20261109T010000Z.trk", "x"), 0o755) // a directory: os.Remove fails
	e.Radio.SetGpsTrackFiles(stuck)
	if r := e.Do("DELETE", "/gps/files", ""); r.Status != 500 {
		t.Errorf("DELETE of a file that cannot be removed = %d %s", r.Status, r.Body)
	}
}

// cursorOf is a page cursor as the API makes them, over a raw position.
func cursorOf(pos uint64) string {
	return base64.RawURLEncoding.EncodeToString([]byte(strconv.FormatUint(pos, 10)))
}

// A cursor before since (an old plain-time one decodes to 1970) does not
// widen the range: the page starts at since.
func TestTrackCursorStaysInRange(t *testing.T) {
	e := testapi.New(t)
	e.Radio.SetGpsTrack("", minutes(10)...)
	q := "/gps/track?since=2026-11-09T00:02:00Z&until=2026-11-09T00:06:00Z&limit=2"
	old := cursorOf(uint64(t0.UnixMilli()))
	j := e.Do("GET", q+"&cursor="+url.QueryEscape(old), "").JSON(t)
	if data := j["data"].([]any); len(data) != 2 || data[0].(map[string]any)["at"] != "2026-11-09T00:02:00.000Z" {
		t.Fatalf("page from an old cursor = %v", j)
	}
}

// A run of points at one time longer than the cursor's skip count can step
// over stops with an error rather than wrap and repeat points.
func TestTrackRunTooLong(t *testing.T) {
	e := testapi.New(t)
	const skipMax = 1<<16 - 1 // track.go trackCursorSkipMask
	at := t0.UnixMilli()
	pts := make([]rad.GpsPoint, skipMax+2)
	for i := range pts {
		pts[i] = rad.GpsPoint{AtMs: at}
	}
	e.Radio.SetGpsTrack("", pts...)
	q := "/gps/track?since=2026-11-09T00:00:00Z&until=2026-11-09T00:00:00Z&limit=1"
	c := cursorOf(uint64(at)<<16 | skipMax)
	if r := e.Do("GET", q+"&cursor="+url.QueryEscape(c), ""); r.Status != 500 {
		t.Fatalf("page past the skip count = %d %s", r.Status, r.Body)
	}
}
