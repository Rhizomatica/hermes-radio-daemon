package transport

import (
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
)

// ProcDir is where the calls in progress are looked for, a variable for the
// tests.
var ProcDir = "/proc"

// callPIDs are the processes whose name (/proc/<pid>/comm) is one of names.
func callPIDs(names []string) []int {
	ents, err := os.ReadDir(ProcDir)
	if err != nil {
		return nil
	}
	var pids []int
	for _, e := range ents {
		pid, err := strconv.Atoi(e.Name())
		if err != nil {
			continue
		}
		b, err := os.ReadFile(filepath.Join(ProcDir, e.Name(), "comm"))
		if err == nil && contains(names, strings.TrimSpace(string(b))) {
			pids = append(pids, pid)
		}
	}
	return pids
}

// stopPIDs asks each process to end (SIGTERM, which uucico and nncp-call
// answer by hanging up cleanly); returns how many it reached.
func stopPIDs(pids []int) int {
	n := 0
	for _, pid := range pids {
		if syscall.Kill(pid, syscall.SIGTERM) == nil {
			n++
		}
	}
	return n
}
