package ratelimit

import (
	"sync"
	"testing"
	"time"
)

func TestLimiter(t *testing.T) {
	l := &Limiter{}
	for i := 0; i < 5; i++ {
		if ok, _, _ := l.Reserve("k", 5, time.Minute); !ok {
			t.Fatalf("attempt %d: Reserve = false before the limit", i+1)
		}
	}
	if ok, retry, _ := l.Reserve("k", 5, time.Minute); ok {
		t.Fatal("Reserve = true at the limit")
	} else if retry <= 0 || retry > time.Minute {
		t.Fatalf("retry = %v", retry)
	}
	// A different key is unaffected.
	if ok, _, _ := l.Reserve("other", 5, time.Minute); !ok {
		t.Fatal("other key is blocked")
	}
}

func TestLimiterRelease(t *testing.T) {
	l := &Limiter{}
	var tok Token
	for i := 0; i < 5; i++ {
		ok, _, tk := l.Reserve("k", 5, time.Minute)
		if !ok {
			t.Fatalf("attempt %d: Reserve = false before the limit", i+1)
		}
		tok = tk
	}
	l.Release(tok)
	if ok, _, _ := l.Reserve("k", 5, time.Minute); !ok {
		t.Fatal("Reserve = false after Release")
	}
	if ok, _, _ := l.Reserve("k", 5, time.Minute); ok {
		t.Fatal("Reserve = true past the limit")
	}
	// Releasing the only event drops the key.
	_, _, tk := l.Reserve("solo", 5, time.Minute)
	l.Release(tk)
	if _, ok := l.hits["solo"]; ok {
		t.Fatal("empty key was not dropped by Release")
	}
	l.Release(Token{}) // a zero token must not panic
}

// Release removes the reservation it names, not the most recent one, so a
// successful attempt does not drop a concurrent attempt's count.
func TestLimiterReleaseSpecific(t *testing.T) {
	l := &Limiter{}
	okA, _, tokA := l.Reserve("k", 5, time.Minute)
	if !okA {
		t.Fatal("first Reserve = false")
	}
	_, _, tokB := l.Reserve("k", 5, time.Minute)
	// Release the first reservation, while the second is still the most
	// recent one.
	l.Release(tokA)
	if got := len(l.hits["k"]); got != 1 {
		t.Fatalf("hits = %d, want 1", got)
	}
	if l.hits["k"][0].id != tokB.id {
		t.Fatal("the wrong reservation was removed")
	}
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
			if ok, _, _ := l.Reserve("k", 5, time.Minute); ok {
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
	l.hits = map[string][]reservation{"k": {{at: old, id: 1}}}
	ok, _, tk := l.Reserve("k", 5, time.Minute)
	if !ok {
		t.Fatal("Reserve = false")
	}
	if got := len(l.hits["k"]); got != 1 {
		t.Fatalf("hits = %d, want only the new one", got)
	}
	l.Release(tk)
	if _, ok := l.hits["k"]; ok {
		t.Fatal("empty key was not evicted")
	}
}
