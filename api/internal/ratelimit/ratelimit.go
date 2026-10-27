// Package ratelimit is a tiny in-memory, sliding-window rate limiter. One
// process holds the session table (api-rest.yaml), so no shared store is
// needed; a restart simply forgets the counters.
package ratelimit

import (
	"sync"
	"time"
)

// Limiter tracks event timestamps per key and reports when a key has hit n
// events within window.
type Limiter struct {
	mu   sync.Mutex
	hits map[string][]time.Time
}

// Allow reports whether key may act now: true when fewer than n events fell
// in the last window. When false, retry is how long until the oldest event
// ages out.
func (l *Limiter) Allow(key string, n int, window time.Duration) (bool, time.Duration) {
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
	if len(keep) == 0 {
		delete(l.hits, key)
	} else {
		l.hits[key] = keep
	}
	if len(keep) >= n {
		return false, time.Until(keep[0].Add(window))
	}
	return true, 0
}

// Hit records one event for key.
func (l *Limiter) Hit(key string) {
	l.mu.Lock()
	defer l.mu.Unlock()
	if l.hits == nil {
		l.hits = map[string][]time.Time{}
	}
	l.hits[key] = append(l.hits[key], time.Now())
}
