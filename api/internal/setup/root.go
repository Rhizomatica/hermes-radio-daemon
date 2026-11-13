package setup

import (
	"context"
	"database/sql"
	"errors"

	"github.com/Rhizomatica/hermes-radio-daemon/api/internal/store"
)

// EnsureRoot seeds the station's root recovery account when it is missing.
// On a fresh database it also marks first-boot setup done, so the station
// is reachable as soon as it is installed. An existing account is left as
// it is. Reports whether it created root.
func EnsureRoot(ctx context.Context, st *store.Store) (bool, error) {
	if _, err := st.UserByUsername(ctx, store.RootUsername); err == nil {
		return false, nil
	} else if !errors.Is(err, sql.ErrNoRows) {
		return false, err
	}

	n, err := st.CountUsers(ctx)
	if err != nil {
		return false, err
	}
	if n == 0 {
		_, err = st.CompleteSetup(ctx, store.NewUser{
			Username: store.RootUsername, PasswordHash: rootSeedHash, Role: "admin",
		}, map[string]string{"setup.done": "1"})
		if err == nil {
			return true, nil
		}
		if !errors.Is(err, store.ErrSetupDone) {
			return false, err
		}
		// Setup won the race and may have created another admin; seed root
		// below if that admin is not root.
	}

	_, err = st.CreateUser(ctx, store.NewUser{
		Username: store.RootUsername, PasswordHash: rootSeedHash, Role: "admin",
	})
	if errors.Is(err, store.ErrUsernameTaken) {
		return false, nil
	}
	return err == nil, err
}
