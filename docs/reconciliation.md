# Compact reconciliation

`littlebear/reconcile.h` is an independent, header-only implementation
of range reconciliation for records with independent 64-bit IDs and fingerprints.
It does not include or wrap upstream Negentropy. The vendored engine and C wrapper have been removed.

## Identity and storage contract

An ID is the chosen 64-bit record key. Its fingerprint is a separate 64-bit
value already maintained by the data structure. Content can change while the
ID remains stable. IDs are sorted as unsigned integers; zero and UINT64_MAX
are representable. Fingerprints may also be zero or UINT64_MAX.

Range summaries use the data structure's existing XOR of stored fingerprints.
The engine neither hashes IDs nor computes a second hash or changes the
fingerprint codec. Peers must agree on fingerprint semantics and encoding.
The existing probabilistic XOR comparison, including possible algebraic
collisions, remains in force. Counts guide subdivision; they are not mixed into
fingerprints or sent as an additional range-equality check.

Explicit lists carry `(id, fingerprint)` pairs. The initiator reports local-only
IDs in `have`, remote-only IDs in `need`, and a shared ID with different
fingerprints in **both**. Matching pairs produce no difference. The driver
uses namespace authority to choose the content version; an ID occurring in both
lists represents differing content, not an instruction to delete the record.

The source supplies three callbacks: cached range count/XOR summaries, selection
of an ID by global rank, and bounded enumeration of records sorted by unique ID.
Counts choose between explicit ID lists (at most 32 records) and subdivision. Each
subdivision produces 16 contiguous groups containing `n/16` records, with one
extra record in the first `n%16` groups. Rank selection supplies boundaries from
the actual IDs. This balances even tightly clustered IDs and uses the tree's
existing count accumulator; no new per-record metadata is needed. Counts remain
local and are not transmitted or mixed into the fingerprint.

The first and last groups retain the original range's outer bounds. Gaps with
no local records still belong to a group, so peer-only IDs in those gaps are
reconciled. A split selects 15 boundaries and obtains 16 child summaries. It
does not enumerate the whole range or allocate a vector of cuts.

`index_sync.h` adapts the existing shared wide index without building a
second in-memory copy. It uses the existing count/XOR summaries and rank
selection directly, and fills bounded item arrays without allocating.
`record.key` supplies the ID and `record.hash` supplies the fingerprint; the two
need not be equal. `record.value` remains the packed payload reference. The
existing key-ordered storage tree can therefore be used directly, without an
additional fingerprint-keyed index. Transfer and payload lookup remain driver
integration work.

Keep the source snapshot and callback context stable for the entire exchange.
In particular, a borrowed namespace-table header can move during table growth.
Use a pinned view or a driver that detects changes and restarts, and do not mutate
the source while applying returned differences. This is an eventual-consistency
mechanism, not a conflict-resolution or ownership policy.

## Memory and processing

All engine storage belongs to the caller. The header performs no heap allocation,
has no standard-container or crypto dependency, and adds no persistent metadata
per record. Production functions are namespace-scoped with plain state structs.

- Workspace: 1,680 bytes, reusable across sequential calls/exchanges.
- IDs and wire fingerprints: 8 bytes each. An explicit item is 16 bytes. A local
  summary returns both the 8-byte count and the 8-byte XOR.
- Recommended output packet buffer: 4 KiB; 535 bytes is enough for any indivisible
  response unit. Larger buffers reduce packet/chunk overhead.
- Difference arrays: 32 IDs per side (512 bytes total) suffice for forward progress.
- Cursor: retains the position within an input frame, the previous boundary,
  and a partially enumerated range. No allocated traversal stack or queue is needed.

`begin` emits a small set directly as an ID offer or starts a larger set with one
summary. `receive` accepts an initiator/responder role, an input packet and cursor,
workspace, output buffer, difference buffers, and a work budget (default 64 range
visits). Only the initiator receives have/need results. Both directions can refine
ranges; a responder can stream answers to an offered range, and an initiator can
compare a complete remote list against arbitrarily many local IDs incrementally.
Enumeration advances through at most 32 local records per chunk, using the next
ID as its boundary. Large unchanged namespaces only query the cached root summary;
they neither enumerate IDs nor select split points.

Small sorted lists are compared by a linear merge. Parsing borrows packet bytes;
encoding appends directly to the caller's buffer. The engine does not construct
strings, hash sets, output vectors, or per-ID allocations.

`more`, `output_full`, and `differences_full` are resumable. Drain the results
already produced, keep the same packet/cursor, and call again. On insufficient
space for even one unit, provide larger buffers. A cursor may make progress
within a frame without changing its byte offset. Never equate an empty partial
output chunk with completion. Input must not overlap workspace or result buffers.
Errors leave the source unchanged; discard the failed exchange's output.

The transport must frame and group streamed chunks into a logical response.
Only after all incoming chunks have been consumed and no response frames remain
can a direction finish. The benchmark coalesces chunks for in-memory transport;
the header does not impose socket framing, namespace status, authentication,
record transfer, or ownership semantics. Those remain driver/protocol work.

## Wire format, version 3

Each packet begins with `4c 42 52 03` (`LBR` plus version 3). Versions 1 and 2,
which equated IDs with fingerprints, and upstream's `0x61` marker are rejected.
Both peers must use v3 to exchange independent IDs and fingerprints.
A packet contains zero or more frames:

| Field | Encoding |
| --- | --- |
| Kind | One byte: 0 summary, 1 offer, 2 answer |
| Boundary widths | One byte: high nibble = lower-bound width, low nibble = upper-bound width |
| Lower bound | 0–8 big-endian bytes; width 9 reuses the preceding frame's finite upper bound |
| Upper bound | 0–8 big-endian bytes; zero represents infinity |
| Summary payload | Exactly 8 bytes, big-endian XOR |
| Offer/answer payload | One-byte count (0–32), then pairs of ID suffix and 8-byte fingerprint |

Ranges are `[low, high)`. An infinite upper bound includes UINT64_MAX. Bounds use
minimal unsigned integer encodings, with zero taking no bytes. Adjacent frames
omit their repeated lower bound. The first frame of every output chunk writes
its lower bound explicitly, so chunks can be concatenated after stripping their
four-byte packet markers. The decoder retains the preceding upper bound in the
packet cursor, including when retrying a frame after buffer backpressure.

For ID lists, whole high bytes shared by every integer in the range are omitted
from each ID. Singleton ranges identify their ID without any suffix bytes, but
still carry the full 8-byte fingerprint. Fingerprints are never prefix-compressed using
ID bits. Boundaries, IDs, and fingerprints use canonical big-endian wire order.

Malformed kinds, boundary widths, nonminimal/empty/reversed ranges, list lengths,
duplicate/unsorted/out-of-range IDs, truncated fields, and foreign versions are
rejected. The caller controls total input size and processing budget. No input
length can trigger an engine allocation.

## Verification and commands

```sh
make -C littlebear-benchmarks test-reconciliation
make -C littlebear-benchmarks benchmark-reconciliation
make -C littlebear-benchmarks benchmark-reconciliation-reference # optional; OpenSSL + pkg-config
make -C littlebear-benchmarks sanitize-reconciliation  # Linux
```

The normal LittleBear test/sanitizer targets include the engine. When the shared
wide index is present, they also include its integration test. The suite covers
200 randomized exchanges against an exact key-to-fingerprint map, equal/empty/
disjoint and edited sets, skewed/clustered IDs, arbitrary ranges and all ID suffix widths,
both integer endpoints, exact group counts with remainders and nonzero start ranks,
source callback failures, buffer backpressure, resumable work, truncation at each
byte, and 20,000 parser inputs. Same-key edits cover small and large sets,
zero/maximum IDs and fingerprints, and atomic result-buffer backpressure. The real wide-tree test
checks a 3,999-key overlapping workload with 100 same-key content edits.

The allocation-counting benchmark asserts zero C++ allocations during the new
engine's exchanges, after constructing its test source. Its callbacks also do
not allocate. The old implementation is available only as an optional benchmark
reference, fetched by `datafriend-benchmarks/tool/fetch_reference.py` at pinned revision
b3687652922785c6f3b6c00684f51c6977965784 with per-file SHA-256 verification. Downloaded
code and its MIT license live under ignored `datafriend/build/reference/`.
The normal application and backend builds neither fetch nor compile it.

## Separate-ID measurements (2026-09-27)

Version 3, median of nine exchanges, 16,384 records per nonempty peer. IDs and
fingerprints are independent stored values; a content edit preserves the ID.
The benchmark source uses arrays, prefix XORs, and rank selection. Timings exclude
source construction and sockets; they do not measure tree-backed storage costs.
Every scenario makes zero C++ allocations during reconciliation. Wire totals
include the final four-byte acknowledgment and exclude transport framing.

| Scenario | macOS / Clang | Droplet / GCC 13 | Wire bytes | Nonempty directions |
| --- | ---: | ---: | ---: | ---: |
| Equal | 0.17 µs | 0.20 µs | 18 | 1 |
| One replacement ID | 11.88 µs | 22.80 µs | 1,857 | 6 |
| Same-ID content edit | 7.12 µs | 9.37 µs | 1,036 | 6 |
| Half overlap | 2.02 ms | 5.38 ms | 656,671 | 6 |
| Disjoint | 1.95 ms | 5.43 ms | 663,771 | 6 |
| Empty responder | 88.79 µs | 506.18 µs | 25 | 2 |
| Empty initiator | 475.29 µs | 1.31 ms | 260,070 | 2 |

Explicit pairs add eight fingerprint bytes per listed ID compared with v2;
range summaries and the 14-byte initial root message retain their previous size.
The fixed workspace increases by 520 bytes to retain the fingerprints of 33
local and 32 remote items. No persistent index fields or heap allocations were
added. The preserved root-first opening still costs an extra round trip in
these edit/overlap/disjoint cases compared with starting from sixteen summaries.

The full backend test suite passes. Both reconciliation tests also pass Linux
ASan/UBSan under a 160 MiB runtime limit and 75% CPU quota: 2.79 seconds elapsed,
52,224 KiB maximum resident memory. Sanitizer compilation required a separate
256 MiB build limit after the compiler exceeded 160 MiB; this was a compilation
limit, not a reconciliation runtime allocation.

## Historical count-balanced measurements (2026-09-26)

These measurements predate separate IDs and fingerprints. Version 2, median of nine exchanges with 16,384 records per nonempty peer.
The source uses sorted arrays, cached prefix XORs, and array rank selection;
construction and sockets are outside the measurement. Every scenario reports
zero C++ allocations during reconciliation. Wire totals include the final empty
four-byte acknowledgment, but exclude socket framing. Message directions below
count nonempty messages; output chunks are coalesced by the benchmark.

| Scenario | macOS / Clang | Droplet / GCC 13 | Wire bytes | Nonempty directions |
| --- | ---: | ---: | ---: | ---: |
| Equal | 0.33 µs | 0.22 µs | 18 | 1 |
| One replacement | 16.71 µs | 21.27 µs | 1,713 | 6 |
| Half overlap | 2.27 ms | 4.17 ms | 397,895 | 6 |
| Disjoint | 1.85 ms | 3.98 ms | 401,483 | 6 |
| Empty responder | 87.33 µs | 229.85 µs | 25 | 2 |
| Empty initiator | 317.88 µs | 860.58 µs | 128,734 | 2 |

Balanced splitting guarantees group sizes on clustered IDs; it is not a uniform
speed improvement. Against the earlier prefix-only version on this uniformly
hashed workload, explicit selected boundaries increase wire bytes and require
rank-selection lookups. The retained root-first opening still adds a round trip
for the measured edit/overlap/disjoint cases relative to upstream's initial
16-group split. Neither smaller fingerprints nor balanced splitting removes it.
Tree-backed rank-selection costs and actual network latency need end-to-end
measurement; the table uses the in-memory benchmark adapter.

Both reconciliation tests pass on Linux under ASan/UBSan, with a 160 MiB limit
and a 75% CPU quota. Test execution took 0.80 seconds, with maximum resident
memory of 38,528 KiB. The regression suite checks exact balanced counts on tightly
clustered IDs, nonzero starting ranks, retained outer gaps, source failures,
resuming compressed boundaries, and the unchanged 14-byte opening message.

## Historical prefix-splitting measurements (2026-09-26)

The table below records version 1 before switching to count-balanced ranges.
It predates both count-balanced splitting and separate IDs/fingerprints.

Same-machine macOS/Clang `-O3` comparison, median of nine exchanges, 16,384 IDs
per nonempty peer. Sources are constructed before timing and use sorted arrays
with cached prefix accumulators. This isolates reconciliation from storage and
network costs. The reference uses the original 32-byte IDs, 16-byte fingerprints,
and SHA-256; these numbers compare the complete old/new designs, not ID width alone.

| Scenario | Original time | New time | Original wire bytes | New wire bytes | Original C++ allocations | New allocations |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Equal | 10.38 µs | 0.21 µs | 337 | 18 | 52 | 0 |
| One replacement | 91.92 µs | 8.42 µs | 1,984 | 1,096 | 314 | 0 |
| Half overlap | 6.30 ms | 1.39 ms | 635,946 | 320,550 | 119,116 | 0 |
| Disjoint | 5.77 ms | 1.49 ms | 641,479 | 322,518 | 134,968 | 0 |
| Empty responder | 637.12 µs | 194.17 µs | 432 | 25 | 32,838 | 0 |
| Empty initiator | 1.86 ms | 395.42 µs | 524,300 | 122,388 | 65,583 | 0 |

Allocation counts cover C++ `new` during exchanges, excluding source construction
and OpenSSL's internal C allocations. New-engine wire totals include the final
four-byte empty acknowledgment; socket framing is excluded for both engines.
The reference allows unlimited frames; the new engine uses 4 KiB output chunks,
coalesced by the benchmark between message directions.

There is a latency tradeoff: the new engine takes six nonempty message directions
for the edit/overlap/disjoint workloads, while the reference takes four messages
including its final response. Starting with one root summary makes unchanged
namespaces very cheap, but numeric prefix refinement may require more network
rounds than upstream's rank-based partitioning. End-to-end latency needs a real
transport measurement; CPU improvements alone do not establish faster sync.

On the target single-core droplet with GCC 13, the new engine measured 0.16 µs
equal, 16.73 µs one replacement, 3.00 ms half overlap, 3.16 ms disjoint, 447.95 µs
empty responder, and 726.55 µs empty initiator. The upstream reference was not
measured there because OpenSSL development headers were absent. These target
numbers should not be compared directly to the macOS reference timings.

Both new test executables passed Linux ASan/UBSan under a 160 MiB memory limit
and a 75% CPU quota. Combined test execution took 0.63 seconds and reported a
maximum resident set of 38,144 KiB with the reduced sanitizer quarantine.

## Review of the removed implementation

The original used 40-byte timestamp/ID items, 32-byte accumulators, and 16-byte
range fingerprints. Producing a range fingerprint constructed a temporary string
and hashed it with SHA-256. Reconciliation built unordered sets of strings,
allocated hash nodes per remote ID, materialized copied ID results, and repeatedly
constructed temporary output strings. The experimental C wrapper then allocated
individual returned IDs again and did not reliably initialize/clean up results
on every exception path. The new header eliminates that entire wrapper/lifetime
layer rather than adopting it.

The upstream reference B-tree also uses a node-ID unordered map, per-search
breadcrumb vectors, type-erased callbacks, and 4 KiB-ish fixed nodes. These costs
are not present in the reconciliation header. The shared wide tree remains the
production storage direction. Its separate key and hash fields now match the
reconciliation item directly; neither field is redundant.
