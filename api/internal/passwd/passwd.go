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
)

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
	key := argon2.IDKey([]byte(password), salt, time, memory, threads, keyLen)
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
	key := argon2.IDKey([]byte(password), p.salt, p.time, p.memory, p.threads, p.keyLen)
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
			p.memory = uint32(n)
		case "t":
			p.time = uint32(n)
		case "p":
			p.threads = uint8(n)
		default:
			return p, ErrInvalid
		}
	}
	if p.memory == 0 || p.time == 0 || p.threads == 0 {
		return p, ErrInvalid
	}
	salt, err := base64.RawStdEncoding.DecodeString(parts[4])
	if err != nil || len(salt) == 0 {
		return p, ErrInvalid
	}
	key, err := base64.RawStdEncoding.DecodeString(parts[5])
	if err != nil || len(key) == 0 {
		return p, ErrInvalid
	}
	p.salt, p.key, p.keyLen = salt, key, uint32(len(key))
	return p, nil
}
