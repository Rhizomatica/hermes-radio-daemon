package setup

import (
	"context"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/passwd"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
)

// The admin every HERMES station starts with, as hermes-api seeds it: a
// station is reachable as soon as it is installed, with no setup step.
// It is a recovery account: root cannot be changed, disabled or deleted,
// so its password stays the one seeded here.
const (
	DefaultAdminUsername = "root"
	DefaultAdminPassword = "caduceu"
)

// EnsureDefaultAdmin creates the default admin and marks first-boot setup
// done when the station has no users at all. A station that has any user,
// or has completed setup, is left as it is. Reports whether it created one.
func EnsureDefaultAdmin(ctx context.Context, st *store.Store) (bool, error) {
	n, err := st.CountUsers(ctx)
	if err != nil || n > 0 {
		return false, err
	}
	hash, err := passwd.Hash(DefaultAdminPassword)
	if err != nil {
		return false, err
	}
	_, err = st.CompleteSetup(ctx, store.NewUser{
		Username: DefaultAdminUsername, PasswordHash: hash, Role: "admin",
	}, map[string]string{"setup.done": "1"})
	if err == store.ErrSetupDone {
		return false, nil // setup ran before and its admin was deleted since: leave it so
	}
	return err == nil, err
}
