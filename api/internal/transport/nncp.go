package transport

import (
	"context"
	"math"
	"os"
	"strconv"
	"strings"
)

// NNCP's files, variables for the tests.
var (
	// NNCPNeigh holds one directory per neighbour, "self" being this station
	// (the installer's hermes-nncp-mesh writes it).
	NNCPNeigh    = "/etc/nncp/neigh"
	NNCPCallsign = "/etc/nncp-callsign"
)

// nncpSelf is NNCPNeigh's entry for this station.
const nncpSelf = "self"

// nncpUnits are nncp-stat's size units.
var nncpUnits = map[string]float64{"B": 1, "KiB": 1 << 10, "MiB": 1 << 20, "GiB": 1 << 30, "TiB": 1 << 40}

type nncp struct {
	run Runner
}

func (n *nncp) kind() Kind { return NNCP }

func (n *nncp) present() bool {
	fi, err := os.Stat(NNCPNeigh)
	return err == nil && fi.IsDir()
}

func (n *nncp) self() string {
	b, err := os.ReadFile(NNCPCallsign)
	if err != nil {
		return ""
	}
	s := strings.TrimSpace(string(b))
	if !ValidName(s) {
		return ""
	}
	return s
}

func (n *nncp) stations() []Station {
	ents, err := os.ReadDir(NNCPNeigh)
	if err != nil {
		return nil
	}
	var out []Station
	for _, e := range ents {
		if e.IsDir() && e.Name() != nncpSelf && ValidName(e.Name()) {
			out = append(out, Station{Name: e.Name()})
		}
	}
	return out
}

func (n *nncp) sendFile(ctx context.Context, dest, name string, data []byte) (string, error) {
	// "-": the file is read from standard input. NNCP prints no job id.
	_, err := n.run.Run(ctx, data, "nncp-file", "-quiet", "-", dest+":"+name)
	return "", err
}

func (n *nncp) exec(ctx context.Context, dest, handler string, args []string, payload []byte) (string, error) {
	argv := append([]string{"-quiet", dest, handler}, args...)
	_, err := n.run.Run(ctx, payload, "nncp-exec", argv...)
	return "", err
}

func (n *nncp) queue(ctx context.Context) ([]Job, error) {
	out, err := n.run.Run(ctx, nil, "nncp-stat", "-pkt")
	if err != nil {
		return nil, err
	}
	return parseNncpStat(string(out)), nil
}

// parseNncpStat reads `nncp-stat -pkt`: a neighbour's name on a line of its
// own, then its packets, indented: "tx <id> 2.4 MiB (nice: B)". Only what is
// waiting to go out (tx) is queued; NNCP packets are encrypted for their
// recipient, so the queue never says what one carries.
func parseNncpStat(out string) []Job {
	var jobs []Job
	node := ""
	for _, l := range strings.Split(out, "\n") {
		if strings.TrimSpace(l) == "" {
			continue
		}
		if l[0] != ' ' && l[0] != '\t' {
			node = strings.TrimSpace(l)
			continue
		}
		f := strings.Fields(l)
		if node == "" || !ValidName(node) || len(f) < 4 || f[0] != "tx" || !ValidName(f[1]) {
			continue
		}
		size, err := strconv.ParseFloat(f[2], 64)
		unit, ok := nncpUnits[f[3]]
		if err != nil || !ok {
			continue
		}
		jobs = append(jobs, Job{
			ID: jobID(NNCP, node, f[1]), Transport: NNCP, Station: node, Kind: "other",
			Bytes: int64(math.Round(size * unit)),
		})
	}
	return jobs
}

func (n *nncp) cancel(ctx context.Context, station, id string) error {
	jobs, err := n.queue(ctx)
	if err != nil {
		return err
	}
	found := false
	for _, j := range jobs {
		found = found || j.ID == jobID(NNCP, station, id)
	}
	if !found {
		return ErrNotFound
	}
	// -pkt takes the packet names on standard input.
	_, err = n.run.Run(ctx, []byte(id+"\n"), "nncp-rm", "-node", station, "-pkt")
	return err
}

func (n *nncp) call(dest string) error {
	// -autotoss: what the call brings in is handed to its handlers at once.
	if dest == "" {
		return n.run.Start("hermes-nncp-call", "--all", "-autotoss")
	}
	return n.run.Start("hermes-nncp-call", "-autotoss", dest)
}

// processes: /proc/<pid>/comm keeps 15 characters of a name.
func (n *nncp) processes() []string { return []string{"hermes-nncp-cal", "nncp-call"} }
