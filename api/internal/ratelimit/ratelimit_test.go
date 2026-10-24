package ratelimit

import (
	"testing"
	"time"
)

func TestLimiter(t *testing.T) {
	l := &Limiter{}
	for i := 0; i < 5; i++ {
		if ok, _ := l.Allow("k", 5, time.Minute); !ok {
			t.Fatalf("attempt %d: Allow = false before the limit", i+1)
		}
		l.Hit("k")
	}
	if ok, retry := l.Allow("k", 5, time.Minute); ok {
		t.Fatal("Allow = true at the limit")
	} else if retry <= 0 || retry > time.Minute {
		t.Fatalf("retry = %v", retry)
	}
	// A different key is unaffected.
	if ok, _ := l.Allow("other", 5, time.Minute); !ok {
		t.Fatal("other key is blocked")
	}
}
