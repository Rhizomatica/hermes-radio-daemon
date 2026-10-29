// Package passwd hashes and verifies user passwords with Argon2id.
//
// Passwords are stored only as an encoded hash (a PHC-style string that
// carries its own parameters, so the cost can be raised later without
// invalidating existing hashes). Verification runs in constant time.
//
// The memory parameter is deliberately modest: radiod caps the Go heap at
// 64 MiB (see cmd/hermesapi HermesInit), so a login must not ask for more
// than a small slice of it. 19 MiB (t=2, p=1) is OWASP's recommended
// minimum for Argon2id.
package passwd

import (
	"crypto/rand"
	"crypto/subtle"
	"encoding/base64"
	"errors"
	"fmt"
	"strconv"
	"strings"

	"golang.org/x/crypto/argon2"
)

const (
	// argon2id parameters, encoded in each hash.
	memory  = 19456 // KiB
	time    = 2     // passes
	threads = 1     // parallelism
	keyLen  = 32    // bytes
	saltLen = 16    // bytes

	// Upper bounds a stored hash may claim. Anything beyond is absurd — a
	// tampered hash — so Verify refuses it instead of allocating for it.
	// maxMemory stays well under radiod's 64 MiB Go heap cap.
	maxMemory  = 32 * 1024 // KiB
	maxTime    = 8         // passes
	maxThreads = 4         // parallelism
	maxKeyLen  = 64        // bytes
)

// slots bounds how many Argon2id computations run at once. Each one holds
// about 19 MiB, so parallel logins (or a burst of guesses) would otherwise
// stack several of them inside radiod's 64 MiB heap cap. Hash and Verify
// queue for the slot instead.
var slots = make(chan struct{}, 1)

// withSlot runs fn while holding the Argon2id slot.
func withSlot(fn func()) {
	slots <- struct{}{}
	defer func() { <-slots }()
	fn()
}

// ErrInvalid is returned by Hash when the password is not usable (it never
// is today: any string, including empty, hashes) and by Verify's parser for
// a malformed hash.
var ErrInvalid = errors.New("passwd: invalid")

// Hash returns an encoded Argon2id hash of password. It fails only when the
// system's randomness source fails.
func Hash(password string) (string, error) {
	salt := make([]byte, saltLen)
	if _, err := rand.Read(salt); err != nil {
		return "", fmt.Errorf("passwd: %w", err)
	}
	var key []byte
	withSlot(func() { key = argon2.IDKey([]byte(password), salt, time, memory, threads, keyLen) })
	return fmt.Sprintf("$argon2id$v=%d$m=%d,t=%d,p=%d$%s$%s",
		argon2.Version, memory, time, threads,
		base64.RawStdEncoding.EncodeToString(salt),
		base64.RawStdEncoding.EncodeToString(key)), nil
}

// Verify reports whether password matches the encoded hash. A malformed
// hash, or a hash with parameters this build considers absurd, is simply
// false: a login attempt that fails either way is treated the same.
func Verify(encoded, password string) bool {
	p, err := parse(encoded)
	if err != nil {
		return false
	}
	var key []byte
	withSlot(func() { key = argon2.IDKey([]byte(password), p.salt, p.time, p.memory, p.threads, p.keyLen) })
	return subtle.ConstantTimeCompare(key, p.key) == 1
}

type params struct {
	salt    []byte
	key     []byte
	memory  uint32
	time    uint32
	threads uint8
	keyLen  uint32
}

// parse reads the PHC-style string Hash produces:
// $argon2id$v=19$m=...,t=...,p=...$<salt>$<key>.
func parse(encoded string) (params, error) {
	var p params
	parts := strings.Split(encoded, "$")
	if len(parts) != 6 || parts[0] != "" || parts[1] != "argon2id" || parts[2] != "v=19" {
		return p, ErrInvalid
	}
	var mem, tm, par uint64
	for _, kv := range strings.Split(parts[3], ",") {
		k, v, ok := strings.Cut(kv, "=")
		if !ok {
			return p, ErrInvalid
		}
		n, err := strconv.ParseUint(v, 10, 32)
		if err != nil {
			return p, ErrInvalid
		}
		switch k {
		case "m":
			mem = n
		case "t":
			tm = n
		case "p":
			par = n
		default:
			return p, ErrInvalid
		}
	}
	if mem == 0 || tm == 0 || par == 0 {
		return p, ErrInvalid
	}
	if mem > maxMemory || tm > maxTime || par > maxThreads {
		return p, ErrInvalid
	}
	p.memory, p.time, p.threads = uint32(mem), uint32(tm), uint8(par)
	salt, err := base64.RawStdEncoding.DecodeString(parts[4])
	if err != nil || len(salt) == 0 {
		return p, ErrInvalid
	}
	key, err := base64.RawStdEncoding.DecodeString(parts[5])
	if err != nil || len(key) == 0 {
		return p, ErrInvalid
	}
	if len(key) > maxKeyLen {
		return p, ErrInvalid
	}
	p.salt, p.key, p.keyLen = salt, key, uint32(len(key))
	return p, nil
}
