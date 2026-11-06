package transport

import (
	"bytes"
	"context"
	"fmt"
	"os/exec"
	"strings"
	"time"
)

// RunTimeout bounds a queueing program (uux, uucp, nncp-exec, nncp-file,
// uustat, nncp-stat, nncp-rm): each only writes to or reads its spool.
const RunTimeout = 30 * time.Second

// stderrMax is how much of a failing program's standard error an error keeps.
const stderrMax = 256

// ExecRunner runs the programs found on PATH.
type ExecRunner struct{}

func (ExecRunner) Run(ctx context.Context, stdin []byte, name string, args ...string) ([]byte, error) {
	ctx, cancel := context.WithTimeout(ctx, RunTimeout)
	defer cancel()
	cmd := exec.CommandContext(ctx, name, args...)
	if stdin != nil {
		cmd.Stdin = bytes.NewReader(stdin)
	}
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	out, err := cmd.Output()
	if err != nil {
		msg := strings.TrimSpace(stderr.String())
		if len(msg) > stderrMax {
			msg = msg[:stderrMax]
		}
		return out, fmt.Errorf("%s: %w: %s", name, err, msg)
	}
	return out, nil
}

func (ExecRunner) Start(name string, args ...string) error {
	cmd := exec.Command(name, args...)
	if err := cmd.Start(); err != nil {
		return fmt.Errorf("%s: %w", name, err)
	}
	go func() { _ = cmd.Wait() }() // reaped when the call ends
	return nil
}
