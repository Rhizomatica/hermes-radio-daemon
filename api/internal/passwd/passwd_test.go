package passwd

import (
	"bytes"
	"encoding/base64"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	gotime "time"
)

func TestHashAndVerify(t *testing.T) {
	h, err := Hash("correct horse battery staple")
	if err != nil {
		t.Fatalf("Hash: %v", err)
	}
	if !strings.HasPrefix(h, "$argon2id$v=19$m=19456,t=2,p=1$") {
		t.Fatalf("unexpected hash format: %q", h)
	}
	if !Verify(h, "correct horse battery staple") {
		t.Error("Verify with the right password = false")
	}
	if Verify(h, "wrong") {
		t.Error("Verify with the wrong password = true")
	}
	if Verify(h, "") {
		t.Error("Verify with an empty password = true")
	}
}

func TestHashUsesFreshSalt(t *testing.T) {
	a, _ := Hash("same password")
	b, _ := Hash("same password")
	if a == b {
		t.Error("two hashes of the same password are identical")
	}
}

func TestVerifyMalformed(t *testing.T) {
	for _, s := range []string{
		"",
		"$argon2id$v=19$m=19456,t=2,p=1$salt$key", // wrong part count
		"$bcrypt$v=19$m=19456,t=2,p=1$c2FsdA$a2V5",
		"$argon2id$v=18$m=19456,t=2,p=1$c2FsdA$a2V5",
		"$argon2id$v=19$m=19456,t=2,p=1$c2FsdA$a2V5$x",
		"$argon2id$v=19$m=0,t=2,p=1$c2FsdA$a2V5",
		"$argon2id$v=19$m=19456,t=2,p=1$!$!",
	} {
		if Verify(s, "anything") {
			t.Errorf("Verify(%q) = true, want false", s)
		}
	}
}

func TestVerifyRejectsAbsurdParams(t *testing.T) {
	for _, s := range []string{
		"$argon2id$v=19$m=999999999,t=2,p=1$c2FsdA$a2V5", // memory
		"$argon2id$v=19$m=19456,t=99,p=1$c2FsdA$a2V5",    // time
		"$argon2id$v=19$m=19456,t=2,p=99$c2FsdA$a2V5",    // threads
	} {
		if Verify(s, "anything") {
			t.Errorf("Verify(%q) = true, want false", s)
		}
	}
	// A key longer than this build accepts is refused, not allocated for.
	key := base64.RawStdEncoding.EncodeToString(bytes.Repeat([]byte("k"), 65))
	if Verify("$argon2id$v=19$m=19456,t=2,p=1$c2FsdA$"+key, "anything") {
		t.Error("Verify with an oversized key = true, want false")
	}
}

// Parallel callers queue for the Argon2id slot: never more than one at once.
func TestSlotLimitsConcurrency(t *testing.T) {
	var running, peak atomic.Int32
	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			withSlot(func() {
				n := running.Add(1)
				for {
					p := peak.Load()
					if n <= p || peak.CompareAndSwap(p, n) {
						break
					}
				}
				gotime.Sleep(5 * gotime.Millisecond)
				running.Add(-1)
			})
		}()
	}
	wg.Wait()
	if peak.Load() != 1 {
		t.Fatalf("peak concurrency = %d, want 1", peak.Load())
	}
}
