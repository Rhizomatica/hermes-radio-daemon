package auth

import (
	"net/http/httptest"
	"testing"
)

func TestSecureRequest(t *testing.T) {
	// No TLS and no header → the cookie is not Secure.
	if req := httptest.NewRequest("GET", "/", nil); secureRequest(req) {
		t.Fatal("plain request marked secure")
	}

	// A remote client spoofing X-Forwarded-Proto is not trusted.
	req := httptest.NewRequest("GET", "/", nil)
	req.Header.Set("X-Forwarded-Proto", "https")
	req.RemoteAddr = "198.51.100.7:1234"
	if secureRequest(req) {
		t.Fatal("spoofed X-Forwarded-Proto from a remote peer marked secure")
	}

	// A loopback proxy setting X-Forwarded-Proto is trusted.
	req = httptest.NewRequest("GET", "/", nil)
	req.Header.Set("X-Forwarded-Proto", "https")
	req.RemoteAddr = "127.0.0.1:1234"
	if !secureRequest(req) {
		t.Fatal("loopback proxy with X-Forwarded-Proto: https not marked secure")
	}
}
