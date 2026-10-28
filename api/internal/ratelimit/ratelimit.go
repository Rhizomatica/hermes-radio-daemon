// Package ratelimit is a tiny in-memory, sliding-window rate limiter. One
// process holds the session table (api-rest.yaml), so no shared store is
// needed; a restart simply forgets the counters.
package ratelimit

import (
	"sync"
	"time"
)

// Limiter tracks event timestamps per key and reports when a key has hit n
// events within window. Empty keys are dropped, so the map holds only keys
// with an event still inside a window.
type Limiter struct {
	mu   sync.Mutex
	hits map[string][]time.Time
}

// Reserve records one event for key and reports whether it was within the
// limit: true when fewer than n events fell in the last window. Checking and
// recording are one step, so a burst of parallel requests cannot all pass
// before any of them counts. When false nothing is recorded and retry is how
// long until the oldest event ages out.
func (l *Limiter) Reserve(key string, n int, window time.Duration) (bool, time.Duration) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.hits == nil {
		l.hits = map[string][]time.Time{}
	}
	now := time.Now()
	cut := now.Add(-window)
	keep := l.hits[key][:0]
	for _, t := range l.hits[key] {
		if t.After(cut) {
			keep = append(keep, t)
		}
	}
	if len(keep) >= n {
		l.hits[key] = keep
		return false, time.Until(keep[0].Add(window))
	}
	l.hits[key] = append(keep, now)
	return true, 0
}

// Release gives back the most recent event of key, for an attempt that
// should not count (a successful login, a server-side failure).
func (l *Limiter) Release(key string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	h := l.hits[key]
	if len(h) <= 1 {
		delete(l.hits, key)
		return
	}
	l.hits[key] = h[:len(h)-1]
}
