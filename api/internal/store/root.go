package store

// RootUsername is the station's built-in recovery account. It is seeded
// at startup, cannot be deleted or have its role changed, and cannot be
// taken by setup or createUser.
const RootUsername = "root"

// IsReservedUsername reports whether username belongs to the built-in
// account and so cannot be taken by setup or createUser.
func IsReservedUsername(username string) bool { return username == RootUsername }
