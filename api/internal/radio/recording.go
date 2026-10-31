package radio

import (
	"context"
	"encoding/json/jsontext"
	"errors"

	rad "github.com/Rhizomatica/hermes-radio-daemon/api/cgo/radio"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/api"
	"github.com/Rhizomatica/hermes-radio-daemon/api/spec"
)

// recording.start and recording.stop (api-ws.yaml RecordingArgs): WAV files
// on the station, in radiod's recording_dir, as the older protocol records.
// The reply says which recordings are on afterwards.
func registerRecording(rt *api.Router) {
	r := rt.Radio()
	rt.Command("recording.start", spec.RoleOperator, func(_ context.Context, _ *api.Conn, args jsontext.Value) (any, *api.CmdError) {
		return record(r, true, args)
	})
	rt.Command("recording.stop", spec.RoleOperator, func(_ context.Context, _ *api.Conn, args jsontext.Value) (any, *api.CmdError) {
		return record(r, false, args)
	})
}

func record(r rad.Radio, start bool, args jsontext.Value) (any, *api.CmdError) {
	var a struct {
		Stream *string `json:"stream"`
	}
	if cerr := api.DecodeArgs(args, &a); cerr != nil {
		return nil, cerr
	}
	stream := "both"
	if a.Stream != nil {
		stream = *a.Stream
	}
	if stream != "rx" && stream != "tx" && stream != "both" {
		return nil, api.Errf(api.CodeOutOfRange, "stream: rx, tx or both")
	}
	rx, tx, err := r.Recording(start, stream)
	switch {
	case errors.Is(err, rad.ErrIO):
		return nil, api.Errf(api.CodeInternal, "the recording file could not be opened")
	case err != nil:
		return nil, cmdErr(err)
	}
	return map[string]bool{"rx": rx, "tx": tx}, nil
}
