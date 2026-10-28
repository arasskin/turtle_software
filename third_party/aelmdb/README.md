# AELMDB for Datafriend

Pinned upstream: https://github.com/amparore/aelmdb at
`359b89807d638e0f255e012cfe8c64dca33a0c02` (0.2.0). Original file hashes are in
UPSTREAM.json; the original license is included.

Local changes: aggregate add/subtract use bytewise XOR; the persisted format tag
is 0xDFA1 instead of 0xA335. Build with MDB_HASH_SIZE=8. Counts remain ordinary
integers. This format cannot open an upstream additive database. Datafriend uses
a separate, versioned database directory and validates its own codec metadata.
