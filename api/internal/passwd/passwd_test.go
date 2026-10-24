package passwd

import (
	"strings"
	"testing"
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
