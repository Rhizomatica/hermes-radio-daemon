// Integration tests: the real radio_daemon, driven over its interfaces.
//
// Each test starts its own daemon on Hamlib's dummy rig (model 1) with no
// audio device, in a temporary directory with its own core.ini, user.ini
// and TCP ports, and stops it with SIGTERM, requiring a clean shutdown.
// Nothing here needs a radio, a sound card or root.
//
//	cd tests/integration && go test ./...
//	RADIOD_BIN=/path/to/radio_daemon go test ./...   (skip the build)
//
// Copyright (C) 2026 Rhizomatica
// SPDX-License-Identifier: GPL-3.0-or-later

package integration

import (
	"bytes"
	"debug/elf"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"
)

var radiodBin string

func TestMain(m *testing.M) {
	bin, err := findDaemon()
	if err != nil {
		fmt.Fprintln(os.Stderr, "integration:", err)
		os.Exit(1)
	}
	radiodBin = bin
	os.Exit(m.Run())
}

// findDaemon returns $RADIOD_BIN, or builds radio_daemon at the top of the
// tree with make (an up-to-date build costs nothing).
func findDaemon() (string, error) {
	if bin := os.Getenv("RADIOD_BIN"); bin != "" {
		return filepath.Abs(bin)
	}
	top, err := filepath.Abs("../..")
	if err != nil {
		return "", err
	}
	cmd := exec.Command("make", "-C", top, "radio_daemon")
	if out, err := cmd.CombinedOutput(); err != nil {
		return "", fmt.Errorf("make radio_daemon: %v\n%s", err, out)
	}
	return filepath.Join(top, "radio_daemon"), nil
}

// hasMercury reports whether the binary has mercury linked in.
func hasMercury(t *testing.T) bool {
	t.Helper()
	f, err := elf.Open(radiodBin)
	if err != nil {
		t.Fatalf("open %s: %v", radiodBin, err)
	}
	defer f.Close()
	syms, err := f.Symbols()
	if err != nil {
		t.Fatalf("symbols of %s: %v", radiodBin, err)
	}
	for _, s := range syms {
		if s.Name == "mercury_init" && s.Section != elf.SHN_UNDEF {
			return true
		}
	}
	return false
}

// freePort asks the kernel for a free TCP port on 127.0.0.1.
func freePort(t *testing.T) int {
	t.Helper()
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("free port: %v", err)
	}
	defer l.Close()
	return l.Addr().(*net.TCPAddr).Port
}

// freePortPair returns a port p with p and p+1 both free (mercury's TNC
// control and data ports).
func freePortPair(t *testing.T) int {
	t.Helper()
	for i := 0; i < 50; i++ {
		l1, err := net.Listen("tcp", "127.0.0.1:0")
		if err != nil {
			t.Fatalf("free port: %v", err)
		}
		p := l1.Addr().(*net.TCPAddr).Port
		l2, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", p+1))
		l1.Close()
		if err == nil {
			l2.Close()
			return p
		}
	}
	t.Fatal("no two adjacent free ports")
	return 0
}

// Daemon is one running radio_daemon.
type Daemon struct {
	t        *testing.T
	Dir      string
	WSURL    string
	RigAddr  string
	UserINI  string
	cmd      *exec.Cmd
	log      *syncBuffer
	exited   chan struct{}
	exitErr  error
	stopOnce sync.Once
}

// daemonOpts adjusts a daemon before it starts.
type daemonOpts struct {
	// Extra [main] lines for core.ini, e.g. "enable_mercury = 1".
	core []string
	// The profile 0 frequency user.ini starts with.
	freq int
}

const defaultUserINI = `[main]
current_profile = 0
default_profile = 0
default_profile_fallback_timeout = -1
step_size = 100
tone_generation = 0

[profile0]
freq = %d
mode = USB
operating_mode = 0
power_level_percentage = 10
mic_level = 50
rx_level = 100
speaker_level = 50
tx_level = 100
bpf_low = 50
bpf_high = 3000
agc = OFF
compressor = OFF
tx_preemphasis = OFF
noise_reduction = OFF
enable_knob_volume = 1
enable_knob_frequency = 1
enable_ptt = 1
`

// startDaemon starts radio_daemon on the dummy rig and waits until both its
// websocket and rigctl ports accept connections.  The daemon is stopped,
// and its clean shutdown checked, when the test ends.
func startDaemon(t *testing.T, o daemonOpts) *Daemon {
	t.Helper()
	dir := t.TempDir()
	wsPort, rigPort := freePort(t), freePort(t)
	if o.freq == 0 {
		o.freq = 7100000
	}

	core := []string{
		"[main]",
		"radio_backend = hamlib",
		"radio_model = 1",
		"rig_pathname = /dev/null",
		"ptt_type = RIG",
		"enable_websocket = 1",
		fmt.Sprintf("websocket_url = ws://127.0.0.1:%d", wsPort),
		"rig_server_enable = 1",
		fmt.Sprintf("rig_server_port = %d", rigPort),
		"cat_server_enable = 0",
		"enable_shm_control = 0",
		"enable_audio_bridge = 0",
		"recording_dir = " + filepath.Join(dir, "recordings"),
	}
	core = append(core, o.core...)
	coreINI := filepath.Join(dir, "core.ini")
	userINI := filepath.Join(dir, "user.ini")
	writeFile(t, coreINI, strings.Join(core, "\n")+"\n")
	writeFile(t, userINI, fmt.Sprintf(defaultUserINI, o.freq))

	d := &Daemon{
		t:       t,
		Dir:     dir,
		WSURL:   fmt.Sprintf("ws://127.0.0.1:%d/", wsPort),
		RigAddr: fmt.Sprintf("127.0.0.1:%d", rigPort),
		UserINI: userINI,
		log:     &syncBuffer{},
		exited:  make(chan struct{}),
	}
	d.cmd = exec.Command(radiodBin, "-r", coreINI, "-u", userINI, "-c", "-1")
	d.cmd.Dir = dir
	d.cmd.Stdout = d.log
	d.cmd.Stderr = d.log
	if err := d.cmd.Start(); err != nil {
		t.Fatalf("start %s: %v", radiodBin, err)
	}
	go func() {
		d.exitErr = d.cmd.Wait()
		close(d.exited)
	}()
	t.Cleanup(func() { d.Stop() })

	for _, addr := range []string{fmt.Sprintf("127.0.0.1:%d", wsPort), d.RigAddr} {
		if err := d.waitPort(addr, 10*time.Second); err != nil {
			t.Fatalf("%v\n--- daemon log ---\n%s", err, d.Log())
		}
	}
	return d
}

func (d *Daemon) waitPort(addr string, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		select {
		case <-d.exited:
			return fmt.Errorf("radio_daemon exited before %s opened (%v)", addr, d.exitErr)
		default:
		}
		if c, err := net.DialTimeout("tcp", addr, 200*time.Millisecond); err == nil {
			c.Close()
			return nil
		}
		time.Sleep(50 * time.Millisecond)
	}
	return fmt.Errorf("%s did not open within %v", addr, timeout)
}

// Stop sends SIGTERM and requires the daemon to exit 0 with its clean
// shutdown message within 10 s.  Safe to call more than once.
func (d *Daemon) Stop() {
	d.stopOnce.Do(func() {
		select {
		case <-d.exited:
			d.t.Errorf("radio_daemon had already exited (%v)\n--- daemon log ---\n%s", d.exitErr, d.Log())
			return
		default:
		}
		_ = d.cmd.Process.Signal(syscall.SIGTERM)
		select {
		case <-d.exited:
		case <-time.After(10 * time.Second):
			_ = d.cmd.Process.Kill()
			<-d.exited
			d.t.Errorf("radio_daemon ignored SIGTERM for 10 s\n--- daemon log ---\n%s", d.Log())
			return
		}
		if d.exitErr != nil {
			d.t.Errorf("radio_daemon exited with %v\n--- daemon log ---\n%s", d.exitErr, d.Log())
		} else if !strings.Contains(d.Log(), "clean shutdown complete") {
			d.t.Errorf("radio_daemon exited without a clean shutdown\n--- daemon log ---\n%s", d.Log())
		}
	})
}

// Log is everything the daemon wrote to stdout and stderr so far.
func (d *Daemon) Log() string { return d.log.String() }

func writeFile(t *testing.T, path, content string) {
	t.Helper()
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}

// eventually polls cond until it is true or the timeout passes.
func eventually(timeout time.Duration, cond func() bool) bool {
	deadline := time.Now().Add(timeout)
	for {
		if cond() {
			return true
		}
		if time.Now().After(deadline) {
			return false
		}
		time.Sleep(50 * time.Millisecond)
	}
}

// syncBuffer is a bytes.Buffer safe for the daemon's two output pipes and
// the test reading it.
type syncBuffer struct {
	mu  sync.Mutex
	buf bytes.Buffer
}

func (b *syncBuffer) Write(p []byte) (int, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.Write(p)
}

func (b *syncBuffer) String() string {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.buf.String()
}
