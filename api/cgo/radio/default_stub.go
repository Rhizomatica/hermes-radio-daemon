//go:build !radiod

package radio

// Default is a Stub outside radiod (build without the `radiod` tag), so the
// API layer builds and runs its tests with no C code.
func Default() Radio { return NewStub() }
