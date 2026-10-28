# unordered_dense dependency

Upstream: https://github.com/martinus/unordered_dense

Pinned release: **v5.1.0**, commit `b7cf409efee3a4419191bb3db29054b2ff6dc02d`.
Retrieved 2026-09-26. Includes unmodified `include/ankerl/unordered_dense.h`,
`include/ankerl/stl.h`, and MIT license. Used by LittleBear's namespace directory and the hash-table benchmark.
The headers remain unmodified. LittleBear's adapter supplies the prehashed
128-bit key policy, allocation callback and explicit storage ownership contract.

Unlike the 4.x implementation, this version uses groups of sixteen fingerprints
and value indices. Memory accounting uses its `index_bytes()` API, plus value
container capacity and the map object.
