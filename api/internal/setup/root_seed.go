package setup

// rootSeedHash is the Argon2id hash of the documented root recovery
// password, produced with internal/passwd.Hash. The plaintext is not kept
// in the daemon source; root is immutable, so this hash is the one the
// seeded account keeps. It is re-seeded when root is missing, so the
// credential cannot be revoked through the API.
const rootSeedHash = "$argon2id$v=19$m=19456,t=2,p=1$7lEaCJ5515t7qsThZiOKbw$061E4/9/9qlCCeUeTwoA/u7qOFYyKXhnK74M/rQZDSM"
