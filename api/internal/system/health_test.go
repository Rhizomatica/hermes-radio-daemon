package system

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"testing"
)

// fakePinger exercises the health handler without importing the storage package.
type fakePinger struct{ err error }

func (f fakePinger) PingContext(context.Context) error { return f.err }

func TestHealthNoDatabase(t *testing.T) {
	handler := Health(discardLogger(), nil)

	rec := httptest.NewRecorder()
	handler.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/health", nil))

	if rec.Code != http.StatusOK {
		t.Fatalf("GET /health: want status 200, got %d", rec.Code)
	}

	var body map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &body); err != nil {
		t.Fatalf("GET /health: invalid JSON body: %v", err)
	}
	if body["status"] != "ok" {
		t.Fatalf("GET /health: want status ok, got %v", body["status"])
	}
	if _, hasDB := body["database"]; hasDB {
		t.Fatalf("GET /health: unexpected database field without a store: %v", body)
	}
}

func TestHealthDatabaseUp(t *testing.T) {
	handler := Health(discardLogger(), fakePinger{})

	rec := httptest.NewRecorder()
	handler.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/health", nil))

	if rec.Code != http.StatusOK {
		t.Fatalf("GET /health: want status 200, got %d", rec.Code)
	}

	var body map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &body); err != nil {
		t.Fatalf("GET /health: invalid JSON body: %v", err)
	}
	if body["database"] != "ok" {
		t.Fatalf("GET /health: want database ok, got %v", body["database"])
	}
}

func TestHealthDatabaseDown(t *testing.T) {
	handler := Health(discardLogger(), fakePinger{err: errors.New("db down")})

	rec := httptest.NewRecorder()
	handler.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, "/health", nil))

	if rec.Code != http.StatusServiceUnavailable {
		t.Fatalf("GET /health: want status 503, got %d", rec.Code)
	}

	var body map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &body); err != nil {
		t.Fatalf("GET /health: invalid JSON body: %v", err)
	}
	if body["status"] != "degraded" {
		t.Fatalf("GET /health: want status degraded, got %v", body["status"])
	}
	if body["database"] != "unavailable" {
		t.Fatalf("GET /health: want database unavailable, got %v", body["database"])
	}
}

func discardLogger() *slog.Logger {
	return slog.New(slog.NewTextHandler(io.Discard, nil))
}
