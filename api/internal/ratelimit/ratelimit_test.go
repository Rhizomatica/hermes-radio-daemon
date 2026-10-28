package ratelimit

import (
	"sync"
	"testing"
	"time"
)

func TestLimiter(t *testing.T) {
	l := &Limiter{}
	for i := 0; i < 5; i++ {
		if ok, _ := l.Reserve("k", 5, time.Minute); !ok {
			t.Fatalf("attempt %d: Reserve = false before the limit", i+1)
		}
	}
	if ok, retry := l.Reserve("k", 5, time.Minute); ok {
		t.Fatal("Reserve = true at the limit")
	} else if retry <= 0 || retry > time.Minute {
		t.Fatalf("retry = %v", retry)
	}
	// A different key is unaffected.
	if ok, _ := l.Reserve("other", 5, time.Minute); !ok {
		t.Fatal("other key is blocked")
	}
}

func TestLimiterRelease(t *testing.T) {
	l := &Limiter{}
	for i := 0; i < 5; i++ {
		l.Reserve("k", 5, time.Minute)
	}
	l.Release("k")
	if ok, _ := l.Reserve("k", 5, time.Minute); !ok {
		t.Fatal("Reserve = false after Release")
	}
	if ok, _ := l.Reserve("k", 5, time.Minute); ok {
		t.Fatal("Reserve = true past the limit")
	}
	// Releasing the only event drops the key.
	l.Reserve("solo", 5, time.Minute)
	l.Release("solo")
	if _, ok := l.hits["solo"]; ok {
		t.Fatal("empty key was not dropped by Release")
	}
	l.Release("never-seen") // must not panic
}

// A burst of parallel reservations must not exceed the limit: the check and
// the count are one step.
func TestLimiterParallelBurst(t *testing.T) {
	l := &Limiter{}
	var wg sync.WaitGroup
	var mu sync.Mutex
	allowed := 0
	for i := 0; i < 50; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if ok, _ := l.Reserve("k", 5, time.Minute); ok {
				mu.Lock()
				allowed++
				mu.Unlock()
			}
		}()
	}
	wg.Wait()
	if allowed != 5 {
		t.Fatalf("allowed = %d, want 5", allowed)
	}
}

func TestLimiterEvictsEmptyKeys(t *testing.T) {
	l := &Limiter{}
	old := time.Now().Add(-2 * time.Minute)
	l.hits = map[string][]time.Time{"k": {old}}
	if ok, _ := l.Reserve("k", 5, time.Minute); !ok {
		t.Fatal("Reserve = false")
	}
	if got := len(l.hits["k"]); got != 1 {
		t.Fatalf("hits = %d, want only the new one", got)
	}
	l.Release("k")
	if _, ok := l.hits["k"]; ok {
		t.Fatal("empty key was not evicted")
	}
}
