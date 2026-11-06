// Package transport puts the station's store-and-forward transports, UUCP
// and NNCP, behind one interface (docs/api-design/store-and-forward.md).
//
// Each station this one reaches goes over the transport that reaches it: a
// station in NNCP's neighbours over NNCP, one only in UUCP's sys file over
// UUCP; where both list it, Settings.Prefer wins. Every program runs with an
// argument list, never through a shell.
package transport

import (
	"context"
	"errors"
	"sort"
	"strings"
	"time"
)

// Kind is a transport (api-rest.yaml Transport).
type Kind string

const (
	UUCP Kind = "uucp"
	NNCP Kind = "nncp"
)

// The errors the layer answers with.
var (
	ErrUnknownStation = errors.New("no transport reaches that station")
	ErrNotFound       = errors.New("no such job")
	ErrBusy           = errors.New("a call is in progress")
	ErrNotSupported   = errors.New("the transport cannot do that")
	ErrBadName        = errors.New("not a valid name")
)

// Station is a station this one reaches, with the transports that reach it
// and its name on each (NNCP names a neighbour by what UUCP calls its alias).
type Station struct {
	Name    string
	Aliases []string
	Via     []Kind
	Addr    map[Kind]string
}

func (s *Station) known(n string) bool { return s.Name == n || contains(s.Aliases, n) }

// Job is one job waiting in a transport's queue (api-rest.yaml QueueJob).
type Job struct {
	ID        string // "<transport>.<station>.<the transport's own id>"
	Transport Kind
	Station   string
	Kind      string // message, mail, gps, file, other
	Bytes     int64
	QueuedAt  time.Time // zero when the transport doesn't say (NNCP)
	MailTo    []string  // for mail jobs, when the transport says
}

// Settings are the layer's configuration (core.ini [main], api/radiod_api.h
// radiod_api_transport).
type Settings struct {
	// Prefer is the transport for a station both list.
	Prefer Kind
	// UucicoAirOptions go on a uucico call over the radio (e.g. "-Y", the
	// pre-agreed startup); never on one to UucicoTCPSystems.
	UucicoAirOptions []string
	// UucicoTCPSystems are reached over TCP, not the radio (a gateway's
	// central server).
	UucicoTCPSystems []string
}

// DefaultPrefer is Settings.Prefer when none is set.
const DefaultPrefer = NNCP

// Runner runs the transports' programs.
type Runner interface {
	// Run runs name with args, stdin on its standard input, and returns
	// its standard output once it exits.
	Run(ctx context.Context, stdin []byte, name string, args ...string) ([]byte, error)
	// Start starts name with args and returns at once (a call, which takes
	// minutes); it is reaped when it ends.
	Start(name string, args ...string) error
}

// backend is one transport.
type backend interface {
	kind() Kind
	present() bool
	self() string
	stations() []Station
	sendFile(ctx context.Context, dest, name string, data []byte) (string, error)
	exec(ctx context.Context, dest, handler string, args []string, payload []byte) (string, error)
	queue(ctx context.Context) ([]Job, error)
	cancel(ctx context.Context, station, id string) error
	call(dest string) error // "": every station with work queued
	processes() []string    // the process names of its calls (/proc/<pid>/comm)
}

// Layer is the station's transports.
type Layer struct {
	s        Settings
	backends []backend
}

// New returns the layer over the station's UUCP and NNCP, run through run.
func New(s Settings, run Runner) *Layer {
	if s.Prefer == "" {
		s.Prefer = DefaultPrefer
	}
	return &Layer{s: s, backends: []backend{&uucp{s: s, run: run}, &nncp{run: run}}}
}

func (l *Layer) backend(k Kind) backend {
	for _, b := range l.backends {
		if b.kind() == k && b.present() {
			return b
		}
	}
	return nil
}

// Kinds are the transports this station runs.
func (l *Layer) Kinds() []Kind {
	var out []Kind
	for _, b := range l.backends {
		if b.present() {
			out = append(out, b.kind())
		}
	}
	return out
}

// Self is this station's name, from the preferred transport that has one.
func (l *Layer) Self() string {
	for _, k := range []Kind{l.s.Prefer, UUCP, NNCP} {
		if b := l.backend(k); b != nil {
			if s := b.self(); s != "" {
				return s
			}
		}
	}
	return ""
}

// Stations are the stations this one reaches. UUCP's names come first; an
// NNCP neighbour is the same station when its name is a UUCP name or alias.
func (l *Layer) Stations() []Station {
	var out []*Station
	for _, b := range l.backends {
		if !b.present() {
			continue
		}
		for _, s := range b.stations() {
			var st *Station
			for _, o := range out {
				if o.known(s.Name) {
					st = o
					break
				}
			}
			if st == nil {
				st = &Station{Name: s.Name, Addr: map[Kind]string{}}
				out = append(out, st)
			}
			if !containsKind(st.Via, b.kind()) {
				st.Via = append(st.Via, b.kind())
				st.Addr[b.kind()] = s.Name
			}
			for _, a := range s.Aliases {
				if a != st.Name && !contains(st.Aliases, a) {
					st.Aliases = append(st.Aliases, a)
				}
			}
		}
	}
	res := make([]Station, 0, len(out))
	for _, s := range out {
		res = append(res, *s)
	}
	return res
}

// Route is the transport that reaches dest (a name or an alias), the station
// and its name on that transport.
func (l *Layer) Route(dest string) (Kind, Station, string, error) {
	if !ValidName(dest) {
		return "", Station{}, "", ErrBadName
	}
	for _, s := range l.Stations() {
		if !s.known(dest) {
			continue
		}
		k := s.Via[0]
		if containsKind(s.Via, l.s.Prefer) {
			k = l.s.Prefer
		}
		return k, s, s.Addr[k], nil
	}
	return "", Station{}, "", ErrUnknownStation
}

// stationOf is the station a transport's address belongs to ("" none).
func (l *Layer) stationOf(k Kind, addr string, all []Station) string {
	for _, s := range all {
		if s.Addr[k] == addr {
			return s.Name
		}
	}
	return ""
}

// SendFile queues data for dest's inbox, filed under name; returns the job.
func (l *Layer) SendFile(ctx context.Context, dest, name string, data []byte) (string, error) {
	if !ValidFileName(name) {
		return "", ErrBadName
	}
	k, _, addr, err := l.Route(dest)
	if err != nil {
		return "", err
	}
	id, err := l.backend(k).sendFile(ctx, addr, name, data)
	return jobID(k, addr, id), err
}

// Exec queues handler at dest, payload on its standard input; returns the job.
func (l *Layer) Exec(ctx context.Context, dest, handler string, args []string, payload []byte) (string, error) {
	if !ValidName(handler) {
		return "", ErrBadName
	}
	for _, a := range args {
		if !validArg(a) {
			return "", ErrBadName
		}
	}
	k, _, addr, err := l.Route(dest)
	if err != nil {
		return "", err
	}
	id, err := l.backend(k).exec(ctx, addr, handler, args, payload)
	return jobID(k, addr, id), err
}

// Queue is every job waiting, oldest first (NNCP's, which carry no time,
// last). A job's Station is the station's name, its ID the transport's.
func (l *Layer) Queue(ctx context.Context) ([]Job, error) {
	var out []Job
	all := l.Stations()
	for _, b := range l.backends {
		if !b.present() {
			continue
		}
		jobs, err := b.queue(ctx)
		if err != nil {
			return nil, err
		}
		for i := range jobs {
			if n := l.stationOf(b.kind(), jobs[i].Station, all); n != "" {
				jobs[i].Station = n
			}
		}
		out = append(out, jobs...)
	}
	sort.SliceStable(out, func(i, j int) bool {
		a, b := out[i].QueuedAt, out[j].QueuedAt
		if a.IsZero() != b.IsZero() {
			return !a.IsZero()
		}
		return a.Before(b)
	})
	return out, nil
}

// Cancel removes a job by the id Queue gave it.
func (l *Layer) Cancel(ctx context.Context, id string) error {
	k, station, own, ok := splitJobID(id)
	if !ok {
		return ErrNotFound
	}
	b := l.backend(k)
	if b == nil {
		return ErrNotFound
	}
	return b.cancel(ctx, station, own)
}

// Call calls dest now over via ("": the transport that reaches it), or, with
// dest "", every station with work queued, on every transport; returns the
// transports that call. ErrBusy while a call is in progress.
func (l *Layer) Call(dest string, via Kind) ([]Kind, error) {
	if l.Calling() {
		return nil, ErrBusy
	}
	if dest == "" {
		called := []Kind{}
		for _, b := range l.backends {
			if b.present() && (via == "" || via == b.kind()) {
				if err := b.call(""); err != nil {
					return called, err
				}
				called = append(called, b.kind())
			}
		}
		return called, nil
	}
	k, st, addr, err := l.Route(dest)
	if err != nil {
		return nil, err
	}
	if via != "" && via != k {
		if !containsKind(st.Via, via) {
			return nil, ErrNotSupported
		}
		k, addr = via, st.Addr[via]
	}
	if err := l.backend(k).call(addr); err != nil {
		return nil, err
	}
	return []Kind{k}, nil
}

// Calling reports whether a call is in progress, on any transport.
func (l *Layer) Calling() bool {
	return len(callPIDs(l.allProcesses())) > 0
}

// Stop ends the calls in progress; returns how many it stopped.
func (l *Layer) Stop() int {
	return stopPIDs(callPIDs(l.allProcesses()))
}

func (l *Layer) allProcesses() []string {
	var names []string
	for _, b := range l.backends {
		if b.present() {
			names = append(names, b.processes()...)
		}
	}
	return names
}

// jobID is the layer's id for a transport's job ("" for none): the
// transport, the station's name on it, and the transport's own id.
func jobID(k Kind, addr, own string) string {
	if own == "" {
		return ""
	}
	return string(k) + "." + addr + "." + own
}

func splitJobID(id string) (Kind, string, string, bool) {
	parts := strings.SplitN(id, ".", 3)
	if len(parts) != 3 || !ValidName(parts[1]) || parts[2] == "" {
		return "", "", "", false
	}
	k := Kind(parts[0])
	if k != UUCP && k != NNCP {
		return "", "", "", false
	}
	return k, parts[1], parts[2], true
}

// NameMax is the longest station or handler name (api-rest.yaml UucpName).
const NameMax = 32

// ValidName is a station or handler name: 1 to NameMax of A-Z a-z 0-9 _ -.
func ValidName(s string) bool {
	if s == "" || len(s) > NameMax {
		return false
	}
	for _, c := range s {
		if !(c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '_' || c == '-') {
			return false
		}
	}
	return true
}

// FileNameMax is the longest file name (api-rest.yaml FileName).
const FileNameMax = 255

// ValidFileName is a plain file name, no path (api-rest.yaml FileName).
func ValidFileName(s string) bool {
	if s == "" || len(s) > FileNameMax || s[0] == '.' || s[0] == '-' {
		return false
	}
	for _, c := range s {
		if !(c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '_' || c == '-' || c == '.') {
			return false
		}
	}
	return true
}

// validArg is a handler argument: no spaces, quotes or shell characters, so
// it reaches the other station's handler as one word over either transport.
func validArg(s string) bool {
	if s == "" || s[0] == '-' && len(s) == 1 {
		return false
	}
	for _, c := range s {
		if !(c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' || c >= '0' && c <= '9' ||
			strings.ContainsRune("_-.@+=:,/", c)) {
			return false
		}
	}
	return true
}

func contains(list []string, s string) bool {
	for _, x := range list {
		if x == s {
			return true
		}
	}
	return false
}

func containsKind(list []Kind, k Kind) bool {
	for _, x := range list {
		if x == k {
			return true
		}
	}
	return false
}
