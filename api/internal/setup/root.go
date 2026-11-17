package setup

import (
	"context"
	"errors"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/passwd"
	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
)

// rootDefaultPassword is the password the station starts with. Change it
// after the first login (PUT /users/{id}/password).
const rootDefaultPassword = "caduceu"

// EnsureRoot seeds the station's root recovery account when the database
// has no users, marking first-boot setup done at the same time. A station
// that already has any user, or has completed setup, is left as it is.
// Reports whether it created root.
func EnsureRoot(ctx context.Context, st *store.Store) (bool, error) {
	n, err := st.CountUsers(ctx)
	if err != nil || n > 0 {
		return false, err
	}
	hash, err := passwd.Hash(rootDefaultPassword)
	if err != nil {
		return false, err
	}
	_, err = st.CompleteSetup(ctx, store.NewUser{
		Username: store.RootUsername, PasswordHash: hash, Role: "admin",
	}, map[string]string{"setup.done": "1"})
	if errors.Is(err, store.ErrSetupDone) {
		return false, nil // setup ran before and its admin was deleted since: leave it so
	}
	return err == nil, err
}
