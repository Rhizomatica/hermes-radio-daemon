package config

import "testing"

func TestINIValue(t *testing.T) {
	text := "; a comment\n" +
		"[main]\n" +
		"websocket_url = ws://127.0.0.1:9000\n" +
		"api_v2_enable = 1\n" +
		"\n" +
		"[other]\n" +
		"websocket_url = ws://0.0.0.0:9999\n"

	cases := []struct {
		name, section, key, want string
	}{
		{"found", "main", "websocket_url", "ws://127.0.0.1:9000"},
		{"scoped to section", "other", "websocket_url", "ws://0.0.0.0:9999"},
		{"missing key", "main", "missing", "fallback"},
		{"missing section", "nope", "websocket_url", "fallback"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			if got := iniValue(text, c.section, c.key, "fallback"); got != c.want {
				t.Fatalf("iniValue(%q, %q) = %q, want %q", c.section, c.key, got, c.want)
			}
		})
	}
}

func TestPort(t *testing.T) {
	cases := []struct{ ws, want string }{
		{"ws://0.0.0.0:8080", "8080"},
		{"wss://0.0.0.0:8443", "8443"},
		{"ws://0.0.0.0", "8080"}, // radiod's default port
		{"ws://[::1]:9000", "9000"},
		{"not a url", ""},
	}
	for _, c := range cases {
		t.Run(c.ws, func(t *testing.T) {
			cfg := Config{WebsocketURL: c.ws}
			if got := cfg.Port(); got != c.want {
				t.Fatalf("Port() = %q, want %q", got, c.want)
			}
		})
	}
}

func TestAPIURLs(t *testing.T) {
	cases := []struct{ ws, rest, wsurl string }{
		{"ws://0.0.0.0:8080", "http://0.0.0.0:8080/api/v2", "ws://0.0.0.0:8080/api/v2/ws"},
		{"wss://0.0.0.0:8443", "https://0.0.0.0:8443/api/v2", "wss://0.0.0.0:8443/api/v2/ws"},
		{"not a url", "", ""},
	}
	for _, c := range cases {
		t.Run(c.ws, func(t *testing.T) {
			cfg := Config{WebsocketURL: c.ws}
			if got := cfg.RESTURL(); got != c.rest {
				t.Fatalf("RESTURL() = %q, want %q", got, c.rest)
			}
			if got := cfg.WSURL(); got != c.wsurl {
				t.Fatalf("WSURL() = %q, want %q", got, c.wsurl)
			}
		})
	}
}
