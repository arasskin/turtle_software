# Datafriend

Datafriend is the C99 store used directly by the [Sokol demo](../demo-app/README.md).
Dart/Flutter clients are no longer supported. It owns local facts, query indexes,
shared live results, reconciliation state and owner identities. Local edits block
until affected results and native watches are published; disk flushing is asynchronous by default.
The Sokol demo exercises local storage, queries and signed document sync through
the native nonblocking TCP client. The earlier Dart adapter is unsupported.

## Execution and subscriptions

Datafriend executes directly in the application's serialized calling context.
There is no private engine thread, job queue or future per call. Edits commit and
update affected live results before returning; callers then dispatch their UI
notifications. Serialize all engine calls, result reads, network imports and
close operations. Network I/O may run asynchronously, but its received updates
must enter through that same application context. The disk-flush worker only
syncs the file descriptor and never touches queries, indexes or result maps.

Native applications use [`datafriend/db.h`](include/datafriend/db.h): typed
store/result handles, numeric query clauses, atomic edit batches and watches.
Watches dispatch automatically after both local commits and remote imports.
See the [C API guide](../docs/datafriend-c-api.md) for examples and ownership rules.

`df_call` and the optional C++ adapters remain compatibility interfaces. The
application's `datafriend_ui.c` now only adds profiling, error presentation and
text-buffer conversion; it no longer encodes database requests or owns watches.

## Source layout

| Path | Responsibility |
| --- | --- |
| `include/datafriend/db.h`, `src/db.c` | Native values, query watches, direct reads and store options |
| `include/datafriend/c_api.h`, `core.h` | C store operations, retained results, borrowed reads and sync hooks |
| `include/datafriend/network.h`, `identity.h` | C network and secure owner identity APIs |
| `include/datafriend/cpp/` | Optional C++ ownership, exceptions and container adapters |
| `src/engine.c` | Store lifetime, transactions, edit publication and C wire dispatch |
| `src/query.c` | AEV/AVE indexes, query compilation/evaluation and dependency routing |
| `src/result.c` | Retained result lifetime, direct reads and ordered enumeration |
| `src/sync.c` | AELMDB snapshots, reconciliation, staged transfers and delta validation |
| `src/network.c` | Nonblocking sockets, protocol state, scheduling and reconnect |
| `src/flush.c` | Pthread worker for coalesced disk flushes |
| `src/identity.c` | Private signing state and validated owner-write signing |
| `src/common.h`, `store.h` | Internal codecs, dense maps, ownership and module contracts |
| `src/memory.c` | Allocation-failure injection, compiled only in test builds |
| `platform/android/`, `platform/apple/` | C Keystore/Keychain bridges and packaging |
| `schema/`, `tool/`, `include/datafriend/schema.h` | Shared generated attribute IDs/names/fingerprint |
| `tests/` | Direct C, engine, flush, identity, allocator and reconciliation correctness |

The core does not depend on an app, renderer, C++ runtime or benchmark worktree.
AELMDB and Monocypher compile as explicit C dependency targets. Reconciliation
uses `littlebear/src/reconcile.h`; no copied fact tree is built for a sync.
The old C++ implementation files have been removed. `src/async_flush.h` is only
an adapter for the existing worker tests. Wide-tree/slab C++ adapters remain
available for independent index callers; the store uses dense maps.

The engine coordinates storage, query evaluation, results and sync. Values have
one allocation containing their reference count, cached hash and bytes. Indexes
and results share them. Maps have dense 16-byte entries and 32-bit bucket
positions, with backward-shift deletion. Attribute columns use compact generated
schema positions, so gaps in wire attribute IDs do not allocate empty columns.
General, fixed-entity and literal-predicate dependency routing is retained;
entity-local edits only evaluate the affected entities. Failed precommit edits
rebuild the existing live handles from committed storage, or poison reads if
recovery itself fails.

The C++ adapter borrows values as `std::string_view`: `query_result::value()`
returns a pointer to a borrowed descriptor, valid until the next `value()` call
on that result. The bytes remain valid until mutation or release. Box handles
retain their bytes independently. Copy to `std::string` when retaining a plain
borrowed value across edits. C++ query/edit convenience calls encode the existing
C wire API; the C network driver calls internal store functions directly.

CocoaPods compiles the small vendor adapter files because it collects sources
beneath its pod root. CMake compiles the original vendor C files directly.

See the [repository dependency map](../architecture.visualize),
[network contract](../docs/datafriend-network.md),
[C ABI contract](../docs/datafriend-native.md), and [source notices](NOTICE.md).

## Build and check

This repository is the Datafriend component of the DrawDo workspace. The current
build expects sibling `containers/`, `littlebear/`, and `third_party/` directories
from that workspace; it is not yet a self-contained source distribution. Commands
below run from the workspace root. The schema generator also checks the workspace's
Dart compatibility output. Application code and native UI libraries are separate
repositories/directories and are not included here.

```sh
make -C datafriend native
# Library only: no C++ compiler required
cmake -S datafriend -B datafriend/build/c-only -DDATAFRIEND_TESTS=OFF
cmake --build datafriend/build/c-only
make -C datafriend test
# Linux ASan/UBSan:
make -C datafriend sanitize
```

`BUILD=build/native` and `JOBS=2` are configurable. Direct CMake:

```sh
cmake -S datafriend -B datafriend/build/native -DCMAKE_BUILD_TYPE=Release
cmake --build datafriend/build/native -j 4
ctest --test-dir datafriend/build/native --output-on-failure
```

Set `DATAFRIEND_TESTS=OFF` for library-only builds. The target
`datafriend::datafriend` supplies public includes; the shared library remains
`libdatafriend` and the C ABI is unchanged. `Datafriend.podspec` packages Apple
clients. Performance experiments remain in the separate
[datafriend-benchmarks](../datafriend-benchmarks/README.md) and
[littlebear-benchmarks](../littlebear-benchmarks/README.md) worktrees.

CMake defaults to a shared library. The iOS Sokol client sets
`DATAFRIEND_SHARED=OFF` for static linkage and selects `MDB_USE_POSIX_SEM=1` for
AELMDB, since iOS does not support its Darwin-default System V semaphore path.
The CocoaPods integration selects the same semaphore backend.


The native network client is `datafriend/network.h`: attach owner/replica stores,
then call `pump()` on the application thread. It detects local edits by revision,
handles signed writes and server-initiated reconciliation, and dispatches query watches after remote commits. The compatibility subscription
API also supports an explicit changed-ID callback. `datafriend/identity.h` provides the secure native
owner identity. See [API and protocol](../docs/datafriend-network.md).

`build/native/transport_test HOST PORT` exercises the production TCP server with
ephemeral identities and synthetic data. It is separate from CTest because it
requires a running endpoint. The app-level CRDT is intentionally outside this library.

## Shared containers and dynamic channels

The live C implementation uses the header-only primitives in
[`../containers/`](../containers/README.md): dense integer maps, growable storage
and byte buffers. Protocol codecs, query routing and value ownership stay in
Datafriend. The legacy tree adapters use the same shared C tree and pool.

`df_network_push` and `df_network_subscribe` can append channels at runtime,
including during a publication callback. Storage grows geometrically and channel
IDs remain stable across growth and reconnect. The network pump rotates its
starting channel to avoid repeatedly favoring low IDs when the budget expires.
There is no 64-channel cap. Allocation and the nonzero 32-bit channel ID space
bound growth. Calls remain serialized on the application thread; callbacks must
not recursively pump or destroy the network. Detach runs outside pump/callbacks; it releases a store without reconnecting other
channels. Attached stores and signing identities must outlive their attachment.

`channel_test HOST PORT` exercises 140 channels against the real server, including
native-watch growth, failed allocation/retry, late updates, reconnect and
middle/in-flight detachment. `db_test` covers the direct C interface, rollback,
ID allocation, snapshots, watch mutation, joins and allocation failures.
