// Package config reads the API layer's slice of radiod's configuration from
// core.ini, the same file radiod's C side reads. The API layer owns no
// socket: radiod's C server listens on main:websocket_url and hands /api/v2
// requests to the Go exports.
package config

import (
	"net/url"
	"os"
	"strings"
)

// radiod's configuration, mirroring common/cfg_utils.c and radio/radio.h.
const (
	coreINIPath      = "/etc/hermes/core.ini" // CFG_RADIO_PATH in C
	defaultWebsocket = "ws://0.0.0.0:8080"    // main:websocket_url, as C defaults
	defaultDatabase  = "/var/lib/hermes-radio-daemon/hermes.sqlite"
)

// Config is the slice of radiod's configuration the API layer reads.
type Config struct {
	// WebsocketURL is main:websocket_url: radiod's own listener. The API
	// layer never binds it.
	WebsocketURL string
	// DatabasePath is where the API's SQLite database lives. DATABASE_PATH
	// overrides it (the self-check uses this); the default is absolute, not
	// relative to the process working directory.
	DatabasePath string
}

// Load reads core.ini and the one environment override the self-check needs.
// A missing or unreadable core.ini falls back to radiod's C defaults.
func Load() Config {
	ws := defaultWebsocket
	if data, err := os.ReadFile(coreINIPath); err == nil {
		ws = iniValue(string(data), "main", "websocket_url", defaultWebsocket)
	}
	return Config{
		WebsocketURL: ws,
		DatabasePath: getenv("DATABASE_PATH", defaultDatabase),
	}
}

// Port returns the port radiod's listener binds, parsed from websocket_url.
// radiod defaults to 8080 when the URL carries no port (parse_ws_url in
// gui_interface/radio_websocket.c). An unparseable URL returns "".
func (c Config) Port() string {
	u, err := url.Parse(c.WebsocketURL)
	if err != nil || u.Host == "" {
		return ""
	}
	if p := u.Port(); p != "" {
		return p
	}
	return "8080"
}

// RESTURL returns the REST API's base URL: http://host:port/api/v2, or
// https:// when websocket_url is wss://. Host and port come from radiod's
// listener (websocket_url); only the scheme and path change.
func (c Config) RESTURL() string {
	return c.apiURL("http", "https", "/api/v2")
}

// WSURL returns the hermes.v1 websocket URL on the same listener:
// ws://host:port/api/v2/ws, or wss:// when websocket_url is wss://.
func (c Config) WSURL() string {
	return c.apiURL("ws", "wss", "/api/v2/ws")
}

// apiURL rewrites websocket_url's scheme (plain or tls) and path. An
// unparseable URL returns "".
func (c Config) apiURL(plain, tls, path string) string {
	u, err := url.Parse(c.WebsocketURL)
	if err != nil || u.Host == "" {
		return ""
	}
	if u.Scheme == "wss" {
		u.Scheme = tls
	} else {
		u.Scheme = plain
	}
	u.Path = path
	u.RawQuery = ""
	u.Fragment = ""
	return u.String()
}

// iniValue returns the value of section:key in the INI text, or fallback. It
// reads the layout radiod's iniparser uses: [section] headers, key = value
// lines, and ';'/'#' comments.
func iniValue(text, section, key, fallback string) string {
	cur := ""
	for _, line := range strings.Split(text, "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, ";") || strings.HasPrefix(line, "#") {
			continue
		}
		if strings.HasPrefix(line, "[") && strings.HasSuffix(line, "]") {
			cur = strings.TrimSpace(line[1 : len(line)-1])
			continue
		}
		if cur != section {
			continue
		}
		if i := strings.Index(line, "="); i >= 0 && strings.TrimSpace(line[:i]) == key {
			return strings.TrimSpace(line[i+1:])
		}
	}
	return fallback
}

func getenv(key, fallback string) string {
	if v, ok := os.LookupEnv(key); ok && v != "" {
		return v
	}
	return fallback
}
