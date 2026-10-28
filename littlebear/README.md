# Littlebear

Littlebear is a disposable namespace cache. Owners push their authoritative state;
public subscribers receive fact batches and reconcile against the cache when
their full fingerprints disagree. A missing namespace preserves a
subscriber's last known state, while an existing empty namespace clears it.

One Linux io_uring loop drives TCP and file I/O. Namespaces share persistent
connections; the server starts subscriber reconciliation after updates. Record
IDs and fingerprints are separate 8-byte values. The target is one CPU, roughly
500 MB RAM and 10 GB disk. See the [wire protocol and client API](../docs/datafriend-network.md).

## Boundaries

`server.c` is the orchestration point: parse an envelope, select a channel,
verify an owner write, dispatch sync work, and submit I/O. Auth cannot call sync
or storage. Sync cannot access sockets, signing keys, or io_uring.

| File | Responsibility |
| --- | --- |
| `server.c` | Connections, channels, request dispatch, subscription scheduling, io_uring completions |
| `protocol.h` | LBS5 envelope parsing, field validation and limits; input views borrow the frame |
| `auth.h` | Namespace digest, Ed25519 verification, challenge and sequence state |
| `sync.h` | Namespace revisions, reconciliation and record transfer through explicit `lb_service` and `lb_task` state |
| `exchange.h` | Bounded packet/difference collection around the reconciliation algorithm |
| `reconcile.h` | Allocation-free reconciliation using source callbacks |
| `index_sync.h` | Adapter from the record index to reconciliation callbacks |
| `storage.h` | Namespace/record lifecycle, value references and asynchronous I/O state |
| `storage_uring.h` | Translate prepared storage requests into kernel submissions |
| `namespaces.h` | Compact namespace directory; owns relocatable index headers |
| `index.h` | Relocatable 24-byte tree header and explicit-lifetime index API |
| `../containers/tree.h`, `../containers/pool.h` | Shared C99 wide tree and slab node allocator |
| `heap.h`, `../containers/arena.h` | Size-class value-file allocator and its metadata pool |
| `codec.h` | Shared byte reader/writer, also used by Datafriend's native boundary |

The main dependency paths are:

```text
server -> protocol, auth, sync, storage_uring
sync -> exchange, index_sync, storage
exchange -> reconcile, protocol
index_sync -> reconcile, index
storage -> namespaces -> index
storage -> heap -> containers/arena
index -> containers/tree, containers/pool
```

The runtime is C99: structs, prefixed functions, explicit initialization and
cleanup, and error returns. `lb_task` owns the batch, response and file buffers;
the TCP transport prepares up to 16 independent record writes concurrently per
batch, within the shared 64-operation storage pool. Reads and synchronous callers
retain the single-request path. Disconnects
retain submitted buffers until their completions arrive. No coroutines, exceptions,
STL containers or C++ runtime are used by the server.

The shared tree and reconciliation implementations are C99 headers. All callers,
including C++ model tests, use their explicit-lifetime C APIs directly.
Tree records remain 24 bytes, node headers 24 bytes, and slots 8 bytes. Mutations
retain allocation-failure rollback and cached count/XOR summaries.

The namespace directory uses dense entries and a linear-probing array of 32-bit
indices. Entries also own revisions and writer state,
replacing the separate C++ metadata maps. Insertions and removals can move entries;
resumed requests always look them up again by namespace ID.

The entry point supplies the generated `datafriend/schema.h` fingerprint to the
protocol parser. xxHash remains the hash dependency; licensing is recorded in
[NOTICE.md](NOTICE.md). The LBS5/LBR3 formats, signed challenges, optimistic updates,
heartbeat and empty-versus-absent namespace behavior are preserved.

## Build and verify

On Linux, install a C99 compiler, liburing and libsodium development packages.
The existing model tests also require a C++23 compiler:

```sh
python3 datafriend/tool/generate_attributes.py --check
make -C littlebear
littlebear/build/server --values /tmp/littlebear.values --port 7778
make -C littlebear test
make -C littlebear sanitize
make -C littlebear debug
python3 scripts/test_littlebear_tcp.py littlebear/build/server
```

The default build produces the real `build/server`; `debug` produces
`build/server-debug` with ASan/UBSan. `BUILD=build/linux` selects a separate output
directory. Compiler-generated dependency files track the headers each target uses.
The server and the direct C API test compile with `-std=c99 -pedantic-errors`.
The other tests use C++ containers as independent models and call the C APIs directly.
Portable component tests also run on macOS with `CXX=clang++`; real io_uring tests
run on Linux. The [deployment unit](../deploy/littlebear.service) uses the same
server, with an explicit port of 7777. Configure its executable and data paths for your host.

Performance experiments, historical trees/arenas, and reference tests live in the
original DrawDo `littlebear-benchmarks` Git worktree on
branch `benchmarks`. This `main` checkout builds only the server and production
correctness tests. It has no dependencies on either benchmark worktree.

See the [C99 port validation](../docs/validation/littlebear-c99.md) for the tested
configurations and the narrow before/after tree comparison.

## Channel storage

Each connection grows a dense channel array on demand. A shared `ct_map` maps
opaque nonzero 32-bit wire IDs to array positions; sparse IDs do not allocate
sparse storage. Active work holds an index across I/O, not a pointer into a
relocatable array. Unsubscribe removes the mapping, swaps the final entry into
the hole and repairs its index. Disconnect frees the array and map.
Subscription delivery rotates its starting position. The fixed 64-channel cap
is gone; allocation and container/protocol integer ranges remain the bounds.

## Connections and scheduling

TCP connections use stable 128-slot slabs and a free list; there is no fixed
connection-count limit. Slots are reused only after socket, file and cancellation
completions have drained. Slabs and timer-array capacity are retained until
shutdown. File-descriptor limits, available memory and the configured connection
memory budget determine admission.
The supplied systemd unit sets `LimitNOFILE=1048576`; existing installations must
install the updated unit and reload systemd to pick up that setting.

An intrusive FIFO schedules only connections with work. A connection can appear
once in either the ready queue or the submission-capacity wait queue. Each loop
drains completions, expires due timers, runs at most 256 connection turns and
flushes submissions. Record processing yields after 64 records; packet parsing
and reconciliation are still bounded by the existing protocol limits. An indexed
deadline heap holds one entry per live connection, replacing periodic full-client
scans. A namespace subscriber index wakes only interested channels, including
subscriptions established before a namespace exists. Runnable notifications
coalesce; verified updates retain their ordered delivery chain.

After a write passes fingerprint verification, its changes are copied once into
a reference-counted message shared by the current optimistic subscribers. Each
subscription queues references in revision order. Sends use a connection-local
frame header and the shared body without a payload copy per recipient. The body
stays alive through partial sends and cancellation completions, then is freed
after the final recipient releases it. No per-namespace delta history is kept.
The service emits one notification after releasing the write lock: successful
changes carry a borrowed `lb_publication`, while aborts, restarts and unchanged
revisions pass `NULL`. The transport either queues that publication or marks
subscribers for reconciliation, without walking the subscriber list twice.
Successful TCP op6 uploads complete without allocating a reply buffer; rejection
and reconciliation responses remain encoded. The synchronous C API retains its existing success replies.

Each connection may retain at most 16 MiB of logical delivery backlog, including
queued references and its active shared send. If a subscription cannot queue the
next update because that limit or the global allocation budget is reached, only
that subscription drops its unsent chain and receives a fresh reconciliation
opening after existing socket output drains. A revision gap does the same.
Other subscribers keep their queued messages. Fresh subscription/reconciliation
replies supersede older queued messages; unsubscribe and disconnect release them.

Socket buffers are borrowed from size classes and returned after use. The pool
caches at most 8 MiB and trims cached buffers under pressure. The server reads
one request at a time and pauses further reads while processing it or sending
its response. It does not build an unbounded outgoing response queue.

`--memory-budget bytes` defaults to 268435456 (256 MiB). It accounts for connection
slabs, timer/index allocations, subscriptions, socket buffers, shared update
bodies, delivery references, channels and live task buffers. A shared body counts
once toward actual memory, regardless of recipient count. Delivery allocations,
socket allocations and admission are checked before allocation;
task/channel growth is checked at dispatch/processing boundaries. Exceeding the
budget closes the connection doing that work. This is an application allocation
budget, not an RSS limit: temporary parser/reply allocations, allocator overhead,
kernel socket buffers and the separate storage cache are outside it. Packet
limits, the 64-entry io_uring submission queue and the **64 pending storage
operations** remain unchanged. Submission capacity is retried after flushing;
storage-capacity exhaustion parks the task in a FIFO instead of sending a retry
response. The task retains its payload, record position and write lock. After an
operation completes or is cancelled, the oldest waiter fills available slots
within its bounded write window before new work can consume them. Waiting tasks remain covered by the
connection memory budget, idle deadlines and shutdown cleanup. The FIFO uses
separate links so unrelated notifications cannot reorder it. Record/namespace
conflicts and stale-read checks retain their existing behavior.

Batch writes retain the namespace lock until every submitted operation has
completed. Completion tags identify individual stable request slots; partial
writes reuse their existing reservation, including while waiting for additional
capacity. Completed slots can be refilled while other records remain in flight.
Fingerprint verification and the single subscriber update happen only after the
entire batch succeeds. On failure, unsubmitted operations are cancelled and
submitted operations drain before the lock or buffers are released. As before,
an I/O failure can leave partial cache changes; there is no transactional rollback
of the whole batch. No successful batch update is published for that failure.
Admission adapts to the number of active writers: each task can hold up to
`min(16, max(1, pending_limit / active_writers))` reservations. Already-reserved
operations drain normally when this share shrinks. A lone writer overlaps its
records; many independent writers supply concurrency without each reserving
sixteen slots. The window can grow again as other writers finish or disconnect.

Each storage write reserves one cleanup ticket before I/O. Success uses it to
retire the old extent (or returns it for an insertion); failure uses it to release
the new extent. Both outcomes remain allocation-free. Pending requests carry an
intrusive back-link, allowing constant-time removal on completion or cancellation.
Record conflict detection still checks pending operations before admitting I/O.

The internal storage API reports `lb_error_pending_full` separately from
`lb_error_busy`. Asynchronous tasks expose `waiting_capacity` and must resume
after capacity is available. The synchronous `lb_handle` convenience API retains
its retry response when another caller owns all slots; it cannot drive that
caller's I/O. The TCP server uses the asynchronous queue for storage tasks.

Additional Linux transport regression:

```sh
python3 scripts/test_littlebear_connections.py littlebear/build/server
python3 scripts/test_littlebear_wait_queue.py littlebear/build/server
python3 scripts/test_littlebear_delivery.py littlebear/build/server
```

This exercises 2,048 idle connections, concurrent readiness, subscriber fanout,
slow readers, slot reuse, memory pressure, deadline expiry and shutdown with
thousands of pending receives. `make test` and `make sanitize` also include a
portable C99 connection-pool/queue/timer/subscription model test (libsodium headers
are required). The wait-queue transport regression uses `libxxhash0` to construct
signed record batches and checks 192 simultaneous writers, disconnected waiters
and shutdown under storage pressure.
