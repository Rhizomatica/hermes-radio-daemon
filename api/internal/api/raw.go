package api

import (
	"bufio"
	"bytes"
	"context"
	"fmt"
	"net/http"
	"net/textproto"
	"net/url"
	"strings"
	"time"
)

// The C side of radiod hands a request over as plain strings (its HTTP
// server parsed it): these turn them into an *http.Request and record the
// handler's answer, so cmd/hermesapi only converts C types.

// RequestFromRaw builds a request from radiod's parts: rawHeaders is the
// header block ("Name: value\r\n" lines).
func RequestFromRaw(ctx context.Context, method, path, query, rawHeaders, peer string, body []byte) (*http.Request, error) {
	u := &url.URL{Path: path, RawQuery: query}
	r, err := http.NewRequestWithContext(ctx, method, u.String(), bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	tp := textproto.NewReader(bufio.NewReader(strings.NewReader(rawHeaders + "\r\n")))
	if h, err := tp.ReadMIMEHeader(); err == nil {
		r.Header = http.Header(h)
	}
	r.Host = r.Header.Get("Host")
	r.RemoteAddr = peer
	r.ContentLength = int64(len(body))
	return r, nil
}

// Recorder is a minimal http.ResponseWriter that keeps the answer.
type Recorder struct {
	Status int
	header http.Header
	Body   bytes.Buffer
}

func (w *Recorder) Header() http.Header {
	if w.header == nil {
		w.header = http.Header{}
	}
	return w.header
}

func (w *Recorder) WriteHeader(status int) {
	if w.Status == 0 {
		w.Status = status
	}
}

func (w *Recorder) Write(b []byte) (int, error) {
	if w.Status == 0 {
		w.Status = http.StatusOK
	}
	return w.Body.Write(b)
}

// ServeRaw answers one request given as radiod's parts: the status, the
// Content-Type, the other headers as "Name: value\r\n" lines, and the body.
func (rt *Router) ServeRaw(method, path, query, rawHeaders, peer string, body []byte) (int, string, string, []byte) {
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	r, err := RequestFromRaw(ctx, method, path, query, rawHeaders, peer, body)
	rec := &Recorder{}
	if err != nil {
		Problem(rec, (&http.Request{}).WithContext(ctx), http.StatusBadRequest, "bad_request", err.Error(), nil)
	} else {
		func() {
			defer func() { // one bad handler must not take the daemon down
				if p := recover(); p != nil {
					rt.httpLog.Error(fmt.Sprintf("%s %s: the handler panicked", r.Method, r.URL.Path),
						"panic", fmt.Sprint(p), "request_id", RequestID(r))
					rec = &Recorder{}
					Problem(rec, r, http.StatusInternalServerError, "internal", "the handler failed", nil)
				}
			}()
			rt.ServeHTTP(rec, r)
		}()
	}
	if rec.Status == 0 {
		rec.Status = http.StatusOK
	}
	ct := rec.Header().Get("Content-Type")
	var extra strings.Builder
	for k, vs := range rec.Header() {
		if k == "Content-Type" || k == "Content-Length" {
			continue
		}
		for _, v := range vs {
			extra.WriteString(k + ": " + strings.NewReplacer("\r", "", "\n", "").Replace(v) + "\r\n")
		}
	}
	return rec.Status, ct, extra.String(), rec.Body.Bytes()
}
