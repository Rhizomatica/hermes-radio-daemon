package transport

import (
	"bufio"
	"context"
	"fmt"
	"os"
	"regexp"
	"strconv"
	"strings"
	"time"
)

// UUCP's files, variables for the tests.
var (
	UUCPSys    = "/etc/uucp/sys"
	UUCPConfig = "/etc/uucp/config"
	// OutDir holds a file while uucp copies it into its spool (-C).
	OutDir = "/var/spool/hermes/out"
)

// outFileMode is a file handed to uucp: only radiod (root) and uucp read it.
const outFileMode = 0o600

type uucp struct {
	s   Settings
	run Runner
}

func (u *uucp) kind() Kind { return UUCP }

func (u *uucp) present() bool {
	_, err := os.Stat(UUCPSys)
	return err == nil
}

func (u *uucp) self() string {
	for _, f := range configLines(UUCPConfig) {
		if len(f) >= 2 && f[0] == "nodename" {
			return f[1]
		}
	}
	return ""
}

// stations are the sys file's "system <name>" entries, with their "alias" lines.
func (u *uucp) stations() []Station {
	var out []Station
	for _, f := range configLines(UUCPSys) {
		switch {
		case len(f) >= 2 && f[0] == "system" && ValidName(f[1]):
			out = append(out, Station{Name: f[1]})
		case len(f) >= 2 && f[0] == "alias" && len(out) > 0:
			for _, a := range f[1:] {
				if ValidName(a) {
					out[len(out)-1].Aliases = append(out[len(out)-1].Aliases, a)
				}
			}
		}
	}
	return out
}

func (u *uucp) sendFile(ctx context.Context, dest, name string, data []byte) (string, error) {
	if err := os.MkdirAll(OutDir, 0o700); err != nil {
		return "", err
	}
	f, err := os.CreateTemp(OutDir, ".out-*")
	if err != nil {
		return "", err
	}
	path := f.Name()
	defer os.Remove(path) // uucp -C has copied it by the time it returns
	if err := f.Chmod(outFileMode); err == nil {
		_, err = f.Write(data)
	}
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		return "", err
	}
	// -r: queue, don't call; -j: print the job id; -C: copy into the spool.
	out, err := u.run.Run(ctx, nil, "uucp", "-r", "-j", "-C", path, dest+"!~/"+name)
	return uucpJobID(out), err
}

func (u *uucp) exec(ctx context.Context, dest, handler string, args []string, payload []byte) (string, error) {
	// "-": the payload is the command's standard input.
	argv := append([]string{"-r", "-j", "-", dest + "!" + handler}, args...)
	out, err := u.run.Run(ctx, payload, "uux", argv...)
	return uucpJobID(out), err
}

// uucpJobID is the id uucp -j and uux -j print ("gw.C00A6"), without the
// station, which the layer's id carries already.
func uucpJobID(out []byte) string {
	s := strings.TrimSpace(string(out))
	if i := strings.IndexByte(s, '.'); i >= 0 {
		s = s[i+1:]
	}
	if !ValidName(s) {
		return ""
	}
	return s
}

// uustatLine is one line of `uustat -a`:
// "gw.C00A6 gw root 11-06 15:23 Executing rmail a@b (sending 1234 bytes)".
var uustatSize = regexp.MustCompile(`\((?:sending )?(\d+) bytes\)`)

func (u *uucp) queue(ctx context.Context) ([]Job, error) {
	out, err := u.run.Run(ctx, nil, "uustat", "-a")
	if err != nil {
		return nil, err
	}
	return parseUustat(string(out), time.Now()), nil
}

// uustatFields is how many fields a job line starts with: id, system, user,
// date, time, then the action.
const uustatFields = 6

func parseUustat(out string, now time.Time) []Job {
	var jobs []Job
	sc := bufio.NewScanner(strings.NewReader(out))
	for sc.Scan() {
		f := strings.Fields(sc.Text())
		if len(f) < uustatFields+1 {
			continue
		}
		station, own, ok := strings.Cut(f[0], ".")
		if !ok || !ValidName(station) {
			continue
		}
		j := Job{ID: jobID(UUCP, station, own), Transport: UUCP, Station: station, Kind: "other"}
		if t, err := time.ParseInLocation("01-02 15:04", f[3]+" "+f[4], time.Local); err == nil {
			t = t.AddDate(now.Year(), 0, 0)
			if t.After(now) { // the queue has no year: a job queued last December
				t = t.AddDate(-1, 0, 0)
			}
			j.QueuedAt = t
		}
		if m := uustatSize.FindStringSubmatch(sc.Text()); m != nil {
			j.Bytes, _ = strconv.ParseInt(m[1], 10, 64)
		}
		action, what := f[5], f[6]
		switch {
		case action == "Executing" && (what == "rmail" || what == "crmail"):
			j.Kind = "mail"
			for _, a := range f[7:] {
				if strings.HasPrefix(a, "(") {
					break
				}
				j.MailTo = append(j.MailTo, a)
			}
		case action == "Executing" && (what == "dec_sensors" || what == "dec_beacon"):
			j.Kind = "gps"
		case action == "Executing" && what == "dec_message", strings.HasSuffix(what, ".hmp"):
			j.Kind = "message"
		case action == "Sending":
			j.Kind = "file"
		}
		jobs = append(jobs, j)
	}
	return jobs
}

func (u *uucp) cancel(ctx context.Context, station, id string) error {
	if !ValidName(id) {
		return ErrNotFound
	}
	if _, err := u.run.Run(ctx, nil, "uustat", "-k", station+"."+id); err != nil {
		return fmt.Errorf("%w: %v", ErrNotFound, err)
	}
	return nil
}

func (u *uucp) call(dest string) error {
	// -m: report failures by mail, as hermes-api's calls did.
	args := []string{"-m"}
	if dest == "" {
		args = append(args, "-r1")
		if len(u.s.UucicoTCPSystems) == 0 {
			args = append(args, u.s.UucicoAirOptions...)
		}
	} else {
		if !contains(u.s.UucicoTCPSystems, dest) {
			args = append(args, u.s.UucicoAirOptions...)
		}
		args = append(args, "-S", dest)
	}
	return u.run.Start("uucico", args...)
}

func (u *uucp) processes() []string { return []string{"uucico"} }

// configLines are a UUCP or NNCP configuration file's lines, split into
// fields, comments and blank lines left out.
func configLines(path string) [][]string {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil
	}
	var out [][]string
	for _, l := range strings.Split(string(b), "\n") {
		if i := strings.IndexByte(l, '#'); i >= 0 {
			l = l[:i]
		}
		if f := strings.Fields(l); len(f) > 0 {
			out = append(out, f)
		}
	}
	return out
}
