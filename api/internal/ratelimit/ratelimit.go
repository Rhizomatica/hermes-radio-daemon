// Package ratelimit is a tiny in-memory, sliding-window rate limiter. One
// process holds the session table (api-rest.yaml), so no shared store is
// needed; a restart simply forgets the counters.
package ratelimit

import (
	"sync"
	"time"
)

// MaxKeys bounds how many keys a Limiter holds. Keys come from clients (their
// address), so without a bound a client rotating addresses would grow the map
// for as long as the daemon runs.
const MaxKeys = 4096

// Limiter tracks event timestamps per key and reports when a key has hit n
// events within window. Empty keys are dropped, and a new key that would pass
// MaxKeys first sweeps every key's expired events; if the map is still full,
// the new key is refused like one over its limit (failing closed: memory stays
// bounded, and keys already tracked keep working).
type Limiter struct {
	mu   sync.Mutex
	hits map[string][]reservation
	next uint64
}

// reservation is one event: its timestamp for the window and a unique id so
// Release can remove exactly the event it names, even when several requests
// for the same key are in flight.
type reservation struct {
	at time.Time
	id uint64
}

// Token identifies one reservation, handed back by Reserve for Release. A
// zero Token is a no-op.
type Token struct {
	key string
	id  uint64
}

// Reserve records one event for key and reports whether it was within the
// limit: true when fewer than n events fell in the last window. Checking and
// recording are one step, so a burst of parallel requests cannot all pass
// before any of them counts. When false nothing is recorded and retry is how
// long until the oldest event ages out. On success token is the reservation
// to hand back to Release.
func (l *Limiter) Reserve(key string, n int, window time.Duration) (bool, time.Duration, Token) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.hits == nil {
		l.hits = map[string][]reservation{}
	}
	now := time.Now()
	cut := now.Add(-window)
	if _, known := l.hits[key]; !known && len(l.hits) >= MaxKeys {
		l.sweep(cut)
		if len(l.hits) >= MaxKeys {
			return false, window, Token{}
		}
	}
	keep := l.hits[key][:0]
	for _, r := range l.hits[key] {
		if r.at.After(cut) {
			keep = append(keep, r)
		}
	}
	if len(keep) >= n {
		l.hits[key] = keep
		return false, time.Until(keep[0].at.Add(window)), Token{}
	}
	l.next++
	l.hits[key] = append(keep, reservation{at: now, id: l.next})
	return true, 0, Token{key: key, id: l.next}
}

// sweep drops every event at or before cut, and the keys left empty. A
// Limiter is used with one window (one per call site), so cut fits them all.
func (l *Limiter) sweep(cut time.Time) {
	for k, h := range l.hits {
		keep := h[:0]
		for _, r := range h {
			if r.at.After(cut) {
				keep = append(keep, r)
			}
		}
		if len(keep) == 0 {
			delete(l.hits, k)
		} else {
			l.hits[k] = keep
		}
	}
}

// Keys is how many keys the limiter holds (for tests).
func (l *Limiter) Keys() int {
	l.mu.Lock()
	defer l.mu.Unlock()
	return len(l.hits)
}

// Release removes the reservation token identifies: an attempt that should
// not count (a successful login, a server-side failure). It removes exactly
// that event, not the most recent one, so a success cannot drop a concurrent
// attempt's count.
func (l *Limiter) Release(token Token) {
	if token.key == "" || token.id == 0 {
		return
	}
	l.mu.Lock()
	defer l.mu.Unlock()
	h := l.hits[token.key]
	for i, r := range h {
		if r.id == token.id {
			l.hits[token.key] = append(h[:i], h[i+1:]...)
			break
		}
	}
	if len(l.hits[token.key]) == 0 {
		delete(l.hits, token.key)
	}
}
