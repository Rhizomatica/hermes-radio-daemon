// The clients the tests drive radiod with: a small RFC 6455 websocket
// client (standard library only, as mercury's integration tests are), the
// radiod JSON command protocol on top of it, and the rigctl line protocol.
//
// Copyright (C) 2026 Rhizomatica
// SPDX-License-Identifier: GPL-3.0-or-later

package integration

import (
	"bufio"
	"crypto/rand"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"encoding/json/jsontext"
	"encoding/json/v2"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strings"
	"testing"
	"time"
)

// ── websocket ────────────────────────────────────────────────────────

const wsGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

const (
	opContinuation = 0x0
	opText         = 0x1
	opBinary       = 0x2
	opClose        = 0x8
	opPing         = 0x9
	opPong         = 0xA
)

// wsConn is a client websocket: frames it sends are masked, as RFC 6455
// requires, and the server's handshake accept key is checked.
type wsConn struct {
	conn net.Conn
	r    *bufio.Reader
}

func wsDial(rawURL string) (*wsConn, error) {
	c, _, err := wsDialProto(rawURL, "")
	return c, err
}

// wsDialProto dials asking for a subprotocol ("" for none), with no session.
func wsDialProto(rawURL, proto string) (*wsConn, string, error) {
	return wsDialAuth(rawURL, proto, "")
}

// wsDialAuth is wsDialProto with a session token (Authorization: Bearer),
// which api-ws.yaml accepts instead of the hermes_session cookie.
func wsDialAuth(rawURL, proto, token string) (*wsConn, string, error) {
	u, err := url.Parse(rawURL)
	if err != nil {
		return nil, "", err
	}
	if u.Scheme != "ws" {
		return nil, "", fmt.Errorf("only ws:// is supported here, not %q", u.Scheme)
	}
	conn, err := net.DialTimeout("tcp", u.Host, 5*time.Second)
	if err != nil {
		return nil, "", err
	}
	var nonce [16]byte
	if _, err := rand.Read(nonce[:]); err != nil {
		conn.Close()
		return nil, "", err
	}
	key := base64.StdEncoding.EncodeToString(nonce[:])
	path := u.RequestURI()
	protoHdr := ""
	if proto != "" {
		protoHdr = "Sec-WebSocket-Protocol: " + proto + "\r\n"
	}
	authHdr := ""
	if token != "" {
		authHdr = "Authorization: Bearer " + token + "\r\n"
	}
	fmt.Fprintf(conn, "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"+
		"Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n%s%s\r\n", path, u.Host, key, protoHdr, authHdr)

	r := bufio.NewReader(conn)
	_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	resp, err := http.ReadResponse(r, &http.Request{Method: "GET"})
	if err != nil {
		conn.Close()
		return nil, "", fmt.Errorf("handshake: %w", err)
	}
	if resp.StatusCode != http.StatusSwitchingProtocols {
		conn.Close()
		return nil, "", &httpStatusError{resp.StatusCode}
	}
	sum := sha1.Sum([]byte(key + wsGUID))
	if got, want := resp.Header.Get("Sec-WebSocket-Accept"), base64.StdEncoding.EncodeToString(sum[:]); got != want {
		conn.Close()
		return nil, "", fmt.Errorf("handshake: Sec-WebSocket-Accept %q, want %q", got, want)
	}
	_ = conn.SetReadDeadline(time.Time{})
	return &wsConn{conn: conn, r: r}, resp.Header.Get("Sec-WebSocket-Protocol"), nil
}

// httpStatusError is a refused upgrade.
type httpStatusError struct{ status int }

func (e *httpStatusError) Error() string { return fmt.Sprintf("handshake: HTTP %d", e.status) }

func (c *wsConn) writeFrame(op byte, payload []byte) error {
	hdr := []byte{0x80 | op}
	switch n := len(payload); {
	case n < 126:
		hdr = append(hdr, 0x80|byte(n))
	case n < 65536:
		hdr = append(hdr, 0x80|126, byte(n>>8), byte(n))
	default:
		hdr = append(hdr, 0x80|127)
		hdr = binary.BigEndian.AppendUint64(hdr, uint64(n))
	}
	var mask [4]byte
	if _, err := rand.Read(mask[:]); err != nil {
		return err
	}
	hdr = append(hdr, mask[:]...)
	masked := make([]byte, len(payload))
	for i, b := range payload {
		masked[i] = b ^ mask[i%4]
	}
	_, err := c.conn.Write(append(hdr, masked...))
	return err
}

// readMessage returns the next whole data message (text or binary),
// answering pings and joining fragments on the way.
func (c *wsConn) readMessage(deadline time.Time) (op byte, msg []byte, err error) {
	_ = c.conn.SetReadDeadline(deadline)
	var buf []byte
	var msgOp byte
	for {
		var h [2]byte
		if _, err := io.ReadFull(c.r, h[:]); err != nil {
			return 0, nil, err
		}
		fin, frameOp := h[0]&0x80 != 0, h[0]&0x0f
		if h[1]&0x80 != 0 {
			return 0, nil, errors.New("server sent a masked frame")
		}
		n := uint64(h[1] & 0x7f)
		switch n {
		case 126:
			var ext [2]byte
			if _, err := io.ReadFull(c.r, ext[:]); err != nil {
				return 0, nil, err
			}
			n = uint64(binary.BigEndian.Uint16(ext[:]))
		case 127:
			var ext [8]byte
			if _, err := io.ReadFull(c.r, ext[:]); err != nil {
				return 0, nil, err
			}
			n = binary.BigEndian.Uint64(ext[:])
		}
		if n > 16<<20 {
			return 0, nil, fmt.Errorf("frame of %d bytes", n)
		}
		payload := make([]byte, n)
		if _, err := io.ReadFull(c.r, payload); err != nil {
			return 0, nil, err
		}
		switch frameOp {
		case opPing:
			if err := c.writeFrame(opPong, payload); err != nil {
				return 0, nil, err
			}
			continue
		case opPong:
			continue
		case opClose:
			return opClose, payload, io.EOF
		case opText, opBinary:
			msgOp, buf = frameOp, payload
		case opContinuation:
			buf = append(buf, payload...)
		default:
			return 0, nil, fmt.Errorf("unknown opcode %#x", frameOp)
		}
		if fin {
			return msgOp, buf, nil
		}
	}
}

// Close sends a close frame and closes the connection.
func (c *wsConn) Close() error {
	_ = c.writeFrame(opClose, []byte{0x03, 0xe8}) // 1000: normal closure
	return c.conn.Close()
}

// ── radiod's JSON commands ───────────────────────────────────────────

// radiodClient speaks radiod's websocket protocol: {"cmd": ...} in, a
// reply with the same "cmd" out.  The server also pushes state JSON and
// binary spectrum frames at any time; call skips them.
type radiodClient struct {
	t  *testing.T
	ws *wsConn
}

func dialRadiod(t *testing.T, d *Daemon) *radiodClient {
	t.Helper()
	ws, err := wsDial(d.WSURL)
	if err != nil {
		t.Fatalf("websocket %s: %v\n--- daemon log ---\n%s", d.WSURL, err, d.Log())
	}
	c := &radiodClient{t: t, ws: ws}
	t.Cleanup(func() { c.ws.Close() })
	return c
}

// reply is one command's answer.
type reply struct {
	OK     bool           `json:"ok"`
	Cmd    string         `json:"cmd"`
	Status string         `json:"status"`
	Error  string         `json:"error"`
	Value  jsontext.Value `json:"value"`
}

func (r reply) Number(t *testing.T) int64 {
	t.Helper()
	var v int64
	if err := json.Unmarshal(r.Value, &v); err != nil {
		t.Fatalf("%s: value %s is not a number", r.Cmd, r.Value)
	}
	return v
}

func (r reply) String(t *testing.T) string {
	t.Helper()
	var v string
	if err := json.Unmarshal(r.Value, &v); err != nil {
		t.Fatalf("%s: value %s is not a string", r.Cmd, r.Value)
	}
	return v
}

// call sends {"cmd": cmd, extra...} and returns the reply for that cmd.
func (c *radiodClient) call(cmd string, extra map[string]any) reply {
	c.t.Helper()
	req := map[string]any{"cmd": cmd}
	for k, v := range extra {
		req[k] = v
	}
	b, _ := json.Marshal(req)
	if err := c.ws.writeFrame(opText, b); err != nil {
		c.t.Fatalf("send %s: %v", b, err)
	}
	deadline := time.Now().Add(5 * time.Second)
	for {
		op, msg, err := c.ws.readMessage(deadline)
		if err != nil {
			c.t.Fatalf("waiting for the %s reply: %v", cmd, err)
		}
		if op != opText {
			continue // spectrum, audio
		}
		var r reply
		if json.Unmarshal(msg, &r) != nil || r.Cmd != cmd {
			continue // pushed state, or another command's reply
		}
		return r
	}
}

// nextReply returns the next text message that is a reply (it has "ok"),
// skipping pushed state, the hello frame and binary frames.
func (c *radiodClient) nextReply() reply {
	c.t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for {
		op, msg, err := c.ws.readMessage(deadline)
		if err != nil {
			c.t.Fatalf("waiting for a reply: %v", err)
		}
		if op != opText {
			continue
		}
		var probe map[string]jsontext.Value
		if json.Unmarshal(msg, &probe) != nil {
			continue
		}
		if _, ok := probe["ok"]; !ok {
			continue
		}
		var r reply
		_ = json.Unmarshal(msg, &r)
		return r
	}
}

// ── rigctl ───────────────────────────────────────────────────────────

// rigctlClient speaks the rigctld line protocol radiod's rig_server serves.
type rigctlClient struct {
	t    *testing.T
	conn net.Conn
	r    *bufio.Reader
}

func dialRigctl(t *testing.T, d *Daemon) *rigctlClient {
	t.Helper()
	conn, err := net.DialTimeout("tcp", d.RigAddr, 5*time.Second)
	if err != nil {
		t.Fatalf("rigctl %s: %v", d.RigAddr, err)
	}
	c := &rigctlClient{t: t, conn: conn, r: bufio.NewReader(conn)}
	t.Cleanup(func() { c.conn.Close() })
	return c
}

// cmd sends one command line and returns the first reply line.
func (c *rigctlClient) cmd(line string) string {
	c.t.Helper()
	_ = c.conn.SetDeadline(time.Now().Add(5 * time.Second))
	if _, err := fmt.Fprintf(c.conn, "%s\n", line); err != nil {
		c.t.Fatalf("rigctl %q: %v", line, err)
	}
	got, err := c.r.ReadString('\n')
	if err != nil {
		c.t.Fatalf("rigctl %q: %v", line, err)
	}
	return strings.TrimRight(got, "\n")
}

// cmdLines sends one command and returns n reply lines (e.g. "m": mode,
// passband).
func (c *rigctlClient) cmdLines(line string, n int) []string {
	c.t.Helper()
	out := []string{c.cmd(line)}
	for len(out) < n {
		got, err := c.r.ReadString('\n')
		if err != nil {
			c.t.Fatalf("rigctl %q, line %d: %v", line, len(out)+1, err)
		}
		out = append(out, strings.TrimRight(got, "\n"))
	}
	return out
}
