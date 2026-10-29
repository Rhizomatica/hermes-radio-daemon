package api

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"mime"
	"net/http"
	"net/url"
	"strconv"
	"strings"
)

var problemTitles = map[string]string{
	"bad_request":            "Bad request",
	"unauthenticated":        "Unauthenticated",
	"forbidden":              "Forbidden",
	"not_found":              "Not found",
	"method_not_allowed":     "Method not allowed",
	"unsupported_media_type": "Unsupported media type",
	"invalid":                "Validation failed",
	"busy":                   "Busy",
	"not_running":            "Not running",
	"not_supported":          "Not supported",
	"encryption_not_allowed": "Encryption not allowed",
	"rate_limited":           "Rate limited",
	"csrf":                   "CSRF check failed",
	"bad_setup_code":         "Bad setup code",
	"setup_done":             "Setup already done",
	"bad_passphrase":         "Bad passphrase",
	"conflict":               "Conflict",
	"too_large":              "Too large",
	"internal":               "Internal error",
}

type problem struct {
	Type      string              `json:"type"`
	Title     string              `json:"title"`
	Status    int                 `json:"status"`
	Code      string              `json:"code"`
	Detail    string              `json:"detail,omitempty"`
	RequestID string              `json:"request_id,omitempty"`
	Errors    map[string][]string `json:"errors,omitempty"`
}

// Problem answers with an RFC 9457 problem: a stable string code, the
// request id, and per-field messages for forms (errors may be nil).
func Problem(w http.ResponseWriter, r *http.Request, status int, code, detail string, errors map[string][]string) {
	title := problemTitles[code]
	if title == "" {
		title = http.StatusText(status)
	}
	w.Header().Set("Content-Type", "application/problem+json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(problem{
		Type: "https://hermes.radio/problems/" + code, Title: title, Status: status, Code: code,
		Detail: detail, RequestID: RequestID(r), Errors: errors,
	})
}

// Invalid answers 422 with the fields that failed.
func Invalid(w http.ResponseWriter, r *http.Request, f Fields) {
	Problem(w, r, http.StatusUnprocessableEntity, "invalid", "", f)
}

// JSON answers status with v as JSON (204 and no body when v is nil).
func JSON(w http.ResponseWriter, status int, v any) {
	if v == nil {
		w.WriteHeader(status)
		return
	}
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

// Decode reads the JSON body into dst: Content-Type application/json, one
// object, no field the type does not have. On failure it has answered
// (415 or 400) and returns false.
func Decode(w http.ResponseWriter, r *http.Request, dst any) bool {
	if mt, _, _ := mime.ParseMediaType(r.Header.Get("Content-Type")); mt != "application/json" {
		Problem(w, r, http.StatusUnsupportedMediaType, "unsupported_media_type", "send application/json", nil)
		return false
	}
	dec := json.NewDecoder(io.LimitReader(r.Body, 1<<20))
	dec.DisallowUnknownFields()
	if err := dec.Decode(dst); err != nil {
		Problem(w, r, http.StatusBadRequest, "bad_request", jsonErr(err), nil)
		return false
	}
	if dec.More() {
		Problem(w, r, http.StatusBadRequest, "bad_request", "one JSON value expected", nil)
		return false
	}
	return true
}

func jsonErr(err error) string {
	var te *json.UnmarshalTypeError
	if errors.As(err, &te) && te.Field != "" {
		return fmt.Sprintf("%s: expected %s", te.Field, te.Type)
	}
	return err.Error()
}

// Fields collects validation messages per field (the problem's "errors").
type Fields map[string][]string

// Add records a message for a field.
func (f Fields) Add(field, msg string) { f[field] = append(f[field], msg) }

// OK reports whether nothing failed.
func (f Fields) OK() bool { return len(f) == 0 }

// Page is a collection page: {"data": [...], "next_cursor": "..."}.
type Page[T any] struct {
	Data       []T    `json:"data"`
	NextCursor string `json:"next_cursor,omitempty"`
}

// Limit reads ?limit= (default 50, 1..200). False after answering 422.
func Limit(w http.ResponseWriter, r *http.Request) (int, bool) {
	s := r.URL.Query().Get("limit")
	if s == "" {
		return 50, true
	}
	n, err := strconv.Atoi(s)
	if err != nil || n < 1 || n > 200 {
		Invalid(w, r, Fields{"limit": {"an integer from 1 to 200"}})
		return 0, false
	}
	return n, true
}

// EncodeCursor and DecodeCursor make an opaque cursor of a position.
func EncodeCursor(pos uint64) string {
	return base64.RawURLEncoding.EncodeToString([]byte(strconv.FormatUint(pos, 10)))
}

// DecodeCursor reads ?cursor= (0 when absent). False after answering 422.
func DecodeCursor(w http.ResponseWriter, r *http.Request) (uint64, bool) {
	s := r.URL.Query().Get("cursor")
	if s == "" {
		return 0, true
	}
	b, err := base64.RawURLEncoding.DecodeString(s)
	n, err2 := strconv.ParseUint(strings.TrimSpace(string(b)), 10, 64)
	if err != nil || err2 != nil {
		Invalid(w, r, Fields{"cursor": {"not a cursor this station gave"}})
		return 0, false
	}
	return n, true
}

func bytesReader(b []byte) io.Reader { return bytes.NewReader(b) }

// SameOrigin reports whether the request's Origin header names the request's
// host. It is false when Origin is absent, so callers that exempt other
// clients (Bearer tools) must decide before calling it.
func SameOrigin(r *http.Request) bool {
	origin := r.Header.Get("Origin")
	if origin == "" {
		return false
	}
	u, err := url.Parse(origin)
	if err != nil {
		return false
	}
	return strings.EqualFold(u.Host, r.Host)
}

// SessionCookie is the name of the session cookie.
const SessionCookie = "hermes_session"

// HasSessionCookie reports whether the request carries a session cookie, the
// credential a browser attaches by itself, which is what CSRF and the
// WebSocket origin check guard.
func HasSessionCookie(r *http.Request) bool {
	c, err := r.Cookie(SessionCookie)
	return err == nil && c.Value != ""
}
