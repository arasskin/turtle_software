# Littlebear storage API

`littlebear/storage.h` joins the namespace table, mutable wide trees, and
size-class value-file allocator. `storage_uring.h` provides the io_uring submission
and completion adapter. The storage API uses namespace functions and explicit
lifetime management; the dense directory owns its backing arrays through C++.

The index uses the shared ClojureDart-derived hash-ranked wide tree through
`index.h`, with one size-class slab pool per store.
The generic implementation lives in `datafriend/include/datafriend/` and
requires no Dart runtime or OpenSSL. The value-file allocator and I/O state
machine retain their existing formats and completion rules.

Each leaf entry contains a 64-bit key, packed value-file reference and cached
fingerprint. Internal nodes hold separators and direct child pointers. Every
node has a count/XOR summary. Stable key ranks use XXH64(seed 0) over the
big-endian 8-byte key, grouped in four trailing-zero bits. Replacements preserve
rank and topology. `record_index` exposes size, selection by ordinal, range
count/XOR, bounded enumeration and count-balanced split keys.

Namespace headers are relocatable, non-owning 24-byte records: root pointer,
allocator pointer and root rank. The namespace directory uses vendored
`ankerl::unordered_dense` v5.1.0, with a separate group index and a dense array of
40-byte key/header pairs. It reuses the fingerprint's low half for hashing and
compares both halves for equality.
The storage owner destroys all roots before destroying their shared pool,
including the ordinary all-ones namespace key. A temporary stack view accesses
the shared implementation; it does not allocate a tree object per namespace.

Slabs are 64 KiB and allocated lazily by node capacity class. Each class retains
one empty slab; larger requests above 4 KiB use malloc. The pool's `stats()`
reports live requested bytes and whole reserved slabs, including holes, and
`trim()` releases retained empty slabs. Partial slabs remain available for reuse.
The old binary node arena and separate fingerprint mapping are no longer
allocated by storage. They remain in the repository for reference tests.

## Scope and record format

This is an internal API for trusted callers on one event-loop thread. Namespace
IDs are the existing 128-bit table identifiers; record keys are unsigned 64-bit
integers, including zero and `UINT64_MAX`. Values are opaque binary payloads of
1–65,535 bytes. Offset zero is valid. The packed value reference contains a 48-bit
file offset and a 16-bit payload length; file limits above 2^48 are rejected.

Callers supply the current 64-bit tree fingerprint with a write. It must describe
the supplied record consistently. The compact reconciliation engine uses an
8-byte record key as its ID and the separate stored 8-byte fingerprint as its
XOR contribution. The adapter reads this existing index directly. A fingerprint
is not an ownership proof. The [sync driver](datafriend-network.md) verifies Ed25519 owner signatures
before calling this layer, maps public keys to BLAKE2b-128 directory IDs, and
validates canonical record bytes. See [reconciliation](reconciliation.md).

Tree allocation failure is recoverable. Insertion allocates before publishing
split results; deletion reserves join capacity before removing the record.
Failed edits preserve records, counts, fingerprints and shape, although successful
capacity growth can remain for reuse. Optional shrink failures retain capacity.
Replacement does not allocate. Storage reports `no_memory` and rolls back the
new file slot on failed insertion; a failed deletion cancels its prepared
file-release ticket and keeps the old record indexed. No partial edit or abort
is used as the allocation-failure path.

## Namespace and record operations

- `create_namespace` is idempotent and creates an empty namespace explicitly.
  There is no application namespace-count limit. Allocation failure or exhaustion
  of the library's index representation returns `no_memory` with existing entries
  intact. Initial namespace capacity is an optional reserve hint; the default
  allocates lazily.
- `lookup_namespace` distinguishes absent, empty, and populated namespaces.
  Reads never create a namespace.
- `prepare_write` reserves a new file slot and cleanup metadata; successful I/O
  completion inserts or replaces the tree entry. The old value remains indexed
  until the new bytes are completely written. A failed replacement preserves it.
- `prepare_read` pins the indexed value and checks the destination capacity.
  Successful completion reports its payload length in `request.transferred`.
- `erase_record` removes the index entry and returns its file slot. Erasing the
  last record leaves an existing empty namespace.
- `clear_namespace` reclaims records but keeps the namespace entry.
  `remove_namespace` reclaims records and then removes that entry.

Clear/remove process at most 256 records by default, returning `more_work` if
records remain. Call again on a later event-loop turn. Metadata exhaustion can
also leave a partially cleared namespace; remaining entries are valid and the
operation is retryable. Clearing/removal is not an atomic snapshot transition.
These operations return `busy` while that namespace has pending I/O.

Tree replacement updates its leaf value and all affected fingerprints without
allocation or topology changes. Existing rank priorities remain in place. New
record insertion can still fail to allocate tree nodes after its write finishes;
that failure returns the new file slot and leaves the published index unchanged.
Release tickets reserve free-list metadata before mutation or I/O, so rollback
and retirement do not depend on a successful allocation at completion time.

## Asynchronous lifetime and admission

Initialize `store` and `request` with `{}`. Create the store in its final location:
namespace tree headers borrow its allocator address. The store, active requests,
and their buffers must not move or be copied during use. Write buffers must also
remain unchanged until completion. The driver owns request/buffer storage.

The default pending limit is 64 operations. A bounded intrusive list tracks only
active I/O; there is no added bookkeeping per stored record. Mutations serialize
per record, multiple reads can share one record, and operations on different
records can be batched, including records in the same namespace. Requests carry
namespace IDs and re-look up tree headers at completion, so unrelated namespace
growth or deletion cannot invalidate an outstanding request. Dense value growth
may relocate headers even if a subsequent index allocation fails; requests must
never cache pointers into the directory.

The driver flow is:

1. `prepare_read` or `prepare_write` returns `ok` and leaves a ready request.
2. `storage_uring::enqueue` reserves an SQE and marks the request submitted. If
   the SQ is full, it returns false and leaves the request ready for retry.
3. The event loop submits SQEs, routes storage CQEs to `storage_uring::complete`,
   and marks those CQEs seen itself.
4. `more_io` means a short transfer or retryable `EINTR`/`EAGAIN`: re-enqueue the
   remaining bytes at the advanced offset. Zero progress/EOF is an I/O error.
5. Any terminal completion releases the request from the active list. The driver
   can then reuse or free its request and buffer. Failed reads may have partially
   filled the buffer; consume data only after a successful terminal completion.

`cancel` releases a ready request. A submitted request returns `busy`: the driver
must wait for its original completion, including after kernel cancellation or a
client disconnect. Failed submission must not free buffers referenced by queued
SQEs. `destroy` also returns `busy` until all requests finish. The lower-level
`start_io`/`complete_io` pair allows deterministic tests of the same state machine.

## Cache reset and resource limits

Creation opens a dedicated disposable value file and takes a nonblocking exclusive
lock before truncation. Another store cannot reset that same live file. After
allocation setup succeeds, startup truncates the file and starts with an empty
namespace table and fresh allocation metadata. Cached bytes are never interpreted
as recovered records. Owners must repopulate the cache after restart.

The server now initializes this store on startup with a 1 GiB logical file limit,
replacing the old 45 GiB prototype limit. It does not preallocate that disk space
or require fsync durability. Writes can still fail with ENOSPC and report it.
The file extent is bounded, but OS/page-cache headroom, resident index admission,
and deployment/log retention still need a complete resource policy.

The [server endpoint](datafriend-network.md) connects TCP clients to these record
operations and reconciliation indexes, with public reads and signed owner writes.
Linux uses one io_uring completion loop for sockets and file operations. Portable
component tests use `pread`/`pwrite`; macOS transport tests use a Linux container.
The old echo loop has been removed; the default build uses this endpoint.

## Verification

`make test` runs actual value-file I/O and 5,000 randomized insert/replace/delete
operations against a reference map, checking bytes and full/range fingerprints.
Tests cover binary and maximum-sized values, short transfers, failed partial
writes, read EOF, file capacity, metadata/node exhaustion, rollback/reuse,
cancellation, concurrent reads/writes, table growth/failure and deletion relocation during I/O, bounded clearing,
absent-versus-empty behavior, file locking, and reset on reopen. The tree suite
also checks allocation-free replacement against its structural reference model.

The wide-tree suite additionally checks randomized records/counts/ranges/rank
selection and injects failure at each requested allocation on insertion and
deletion paths, including maximum-rank promotion. Shared-root tests check
namespace-table growth, node reclamation and special-key namespace teardown.
Storage tests exhaust available slab blocks to force a joining deletion to
fail, check that the indexed payload remains readable, and retry successfully.

On Linux the storage test includes real io_uring batched writes, a read, and SQ
exhaustion. Environments denying io_uring report a skip for that part only; set
`LITTLEBEAR_REQUIRE_URING=1` to require kernel integration. It is required for the
on-droplet validation. Normal and sanitizer targets include the storage test;
Linux therefore needs liburing installed for these targets as well as the server.

For the former binary-index version on the one-core droplet, GCC 13.3 reported a
1,856-byte store and 104-byte requests:
64 pending requests need 6,656 bytes, excluding their buffers and allocated
table/arena/heap storage. No new bytes were added to each tree node or namespace
table entry. On 2026-09-26 the storage test, including required real io_uring,
passed ASan/UBSan under a 160 MB memory cap and 75% CPU quota, peaking at
17,408 KiB resident memory. This is a correctness-test footprint, not a loaded
server capacity or throughput measurement.

The slab-wide/dense version uses a 3,224-byte store and the same 104-byte request
on the tested macOS and Linux arm64 builds. Namespace headers remain 24 bytes. The former
slab-wide/two-choice version used a 3,192-byte store and 192-byte buckets. Prior benchmark
numbers predate the allocation-failure changes and are not new production
throughput measurements.

On 2026-09-26 the integration passed the local normal suite and Linux
ASan/UBSan suites in Ubuntu 24.04 arm64 containers limited to one CPU and
512 MiB. A separate storage sanitizer run required real io_uring and passed
batched writes/read/SQ-exhaustion tests with the container syscall filter
disabled. Final join-failure tests and shared-tree assertions were rechecked.
See [the validation record and tested source hashes](validation/littlebear-wide-2026-09-26.json).

The dense migration is covered by allocation-failure sweeps, 50,000 directory
model operations, growth beyond the former 262,144 nominal namespace slots,
and storage I/O across header relocation. The previous integration validation
record above describes its pinned sources; it predates the dense migration.

The dense integration passed the full local normal suite, the Linux server build,
and the full Linux normal and ASan/UBSan suites in a one-CPU, 512 MiB Ubuntu
24.04 arm64 container. Real io_uring was required in the sanitizer run and passed.
See the [dense validation record and source hashes](validation/littlebear-dense-2026-09-26.json).
