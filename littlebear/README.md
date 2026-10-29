# Littlebear

Littlebear is a disposable namespace cache. Owners push their authoritative state;
public subscribers receive fact batches and reconcile against the cache when
their full fingerprints disagree. A missing namespace preserves a
subscriber's last known state, while an existing empty namespace clears it.

One Linux io_uring loop drives TCP, optional WebSocket connections, and file I/O. Namespaces share persistent
connections; the server starts subscriber reconciliation after updates. Record
IDs and fingerprints are separate 8-byte values. The target is one CPU, roughly
500 MB RAM and 10 GB disk. See the [wire protocol and client API](../docs/datafriend-network.md).

## Entry point and operations

`server.c` is the entry point. It directly imports WebSocket framing and all eight
operation files. It owns the two listeners, the transport gate, envelope/schema
validation, channel binding, dispatch, and io_uring scheduling.
Each operation owns its authorization, namespace policy, resumable work, and
completion behavior. Server switches only route calls and supply their arguments.

| Wire ID | Operation file | Decisions owned here |
| --- | --- | --- |
| 0 | `op_0_begin_owner_sync.h` | First reconciliation round, current version, fresh owner intent and challenge |
| 1 | `op_1_continue_reconciliation.h` | Reconciliation and deferred challenge when states differ |
| 2 | `op_2_fetch_records.h` | Fetch keys and recheck the revision across asynchronous reads |
| 3 | `op_3_apply_records_at_revision.h` | Verify owner, require revision, apply, publish, acknowledge |
| 5 | `op_5_close_channel.h` | Remove subscription and release the channel |
| 6 | `op_6_stream_record_changes.h` | Verify owner, allow older revision, check final fingerprint, silent success or reconciliation request |
| 7 | `op_7_heartbeat.h` | Connection-scoped reply without namespace or owner checks |
| 8 | `op_8_subscribe_to_records.h` | Subscription delivery mode and initial opening |

Wire ID 4 is retired and rejected. Operation 8 takes one delivery-mode byte:
`0` for reconciliation notifications, `1` for record batches with reconciliation
fallback. Both modes return an initial reconciliation opening.

Each operation exposes `begin`, `finish`, and (where needed) `resume` functions
with explicit arguments. Network requests always enter through `begin`; the write
operations verify signatures and consume sequences there, before namespace changes.
Server dispatch uses the stable wire IDs and operation rules in `models.h`.

## Shared specification

`models.h` describes the shared world: wire magic and limits, operation IDs and
rules (supported, signed, connection-scoped, empty payload), parsed request views,
response meanings, byte views, publication data, and peer/channel authorization
state. It includes small functions expressing those rules, not just types.
Every component may use it. It imports container primitives, with no dependency
on a Littlebear parser, operation, storage algorithm or transport.

Server privately implements `parse_request_envelope`; operations consume the
parsed `lb_request` model. Encoding lives in `codec.h`, directory observations
in `namespaces.h`, and channel binding/release in `namespace_channels.h`.
The former `request_envelope.h`, `operation_ids.h`, `operation_result.h`, and
`namespace_state.h` have been coalesced into these owners, without forwarding
headers. Operation-private progress and resource management remain local.

The resulting dependency changes are concrete:

- Authorization now imports only `models.h` within Littlebear, plus libsodium;
  it no longer imports parsing or channel management.
- Channel management imports `models.h`; it no longer pulls in the namespace
  directory and index just to describe a peer.
- Operations import the shared specification and their actual tools. Only close
  and subscribe import channel management; the others just use channel models.
- `codec.h` imports models, while models does not depend on encoding.

This deliberately broadens visibility of the specification, while reducing
accidental dependencies on behavior. It does not grant access to state instances;
those still arrive as explicit arguments.

## State ownership

`main` declares the store, a reference to its namespace directory, boot epoch,
active-writer count, buffer pool, subscriber registry, and transport loop. It
initializes their relationships and destroys them after draining connections.
The loop owns listeners, io_uring, connection slabs and scheduling queues.
Connections borrow the separately declared buffer pool and subscriber registry
for transport accounting; they do not initialize or destroy those resources.
The store owns its directory, indexes, pending requests and value-file allocator.

Operations receive only the state they use: reconciliation gets a namespace
directory and epoch; fetching also gets storage; writing additionally gets the
active-writer counter. Channel authorization is passed to `begin`, and channel
and subscription state to `finish`. There is no service object or operation
context, and a request retains neither one nor a resumption callback.

A connection owns its peer/channels and request. The shared request owns copied
payloads, reply buffers, stable I/O slots, scheduling state and outward effects.
Fetch and write operations define their own state structures in their operation
files. The request retains an operation-owned allocation through `state`; only
that operation interprets it or releases its children. Server dispatches cleanup
explicitly, with no stored cleanup callbacks. Operations report retained state
bytes so transport memory accounting does not inspect private fields.
Reconciliation scratch buffers are local to a single call. Namespace
entries are looked up again on resume because their table may move. Channel
pointers are borrowed only during a call. File requests retain their storage
owner for completion; kernel-visible buffers remain pinned until all CQEs arrive.

After unlocking a namespace, a request exposes a `changed` effect and, for a
successful changed batch, a borrowed publication. Server consumes that effect
before disposing the request. Disconnect cleanup drains I/O, calls the write
operation to release its guard, notifies subscribers, then calls operation cleanup
before freeing shared buffers. Subscriber reserve/wake callbacks remain a
transport bridge wired explicitly in `main`; they are not operation dependencies.

## Tools and dependency boundaries

Operations import tools, never other operations, the server, or a transport.
Tools do not import operations. The dependency checker enforces these rules,
checks for include cycles, and verifies server's direct imports.

| Tool | Responsibility |
| --- | --- |
| `websocket.h` | Bounded HTTP upgrade, masking, fragmentation and control frames; directly owned by server |
| `models.h` | Shared concepts, wire constants, request/response contract and operation rules |
| `codec.h` | Encode/decode primitive values and encode response headers |
| `namespace_channels.h` | Bind and release namespace channels; shared peer/channel shapes live in models |
| `owner_authorization.h` | Challenge state, signature verification and replay checks using libsodium; operations encode challenge bytes |
| `write_record_batch.h` | Execute validated batches with bounded concurrent writes; return completion, pending, capacity, conflict or failure |
| `record_batch_codec.h` | Common put/delete decoding, duplicate IDs and fingerprint validation; op2 decodes fetch keys and op6 reads its fingerprint prefix |
| `request_lifetime.h` | Shared request buffers, operation-state allocation, stable I/O slots, drain and outward effects |
| `subscriber_delivery.h` | Namespace watches, shared batches, queue limits and reconciliation fallback |
| `client_connections.h` | Socket state, connection slabs, timers, ready/wait queues and memory accounting |
| `storage_uring.h` | Translate prepared storage requests into Linux kernel submissions |
| `exchange.h`, `reconcile.h`, `index_sync.h` | Bounded reconciliation packets, comparison algorithm and index adapter |
| `storage.h`, `namespaces.h`, `index.h`, `heap.h` | Record storage, namespace directory, index and value-file allocator |

Operations contain their own task setup, response decisions, subscription revision
updates, and completion steps. Op2 owns its key-list decoder and read loop. Op3
and op6 decode the common modification batch, create absent namespaces, acquire
write guards and advance revisions directly in their operation files. They pass
the batch and I/O slots to the executor, interpret its result, and release their
guards before returning publication data. Empty creation advances the revision;
an empty batch against an existing namespace does not.
Op0 and op1 encode their own challenge fields. Small repeated sequences are kept
local so reviewing an operation reveals its behavior without following wrappers.

Rejection and recovery responses also belong to the operation. Each operation
first drains submitted I/O, then builds its response and, for writes, releases
its namespace guard. The shared lifetime code neither chooses nor encodes a
protocol response. The executor cannot create namespaces, change revisions,
release guards, or build replies; it receives no request or namespace directory.
Subscriber delivery builds its own asynchronous opening responses.

For example, a signed streamed update follows these dependencies:

```text
server (envelope parsing) -> models, codec, websocket, op_6_stream_record_changes
op_6_stream_record_changes -> models, owner_authorization
                          -> namespaces, exchange, index_sync
                          -> record_batch_codec
                          -> write_record_batch -> storage, request_lifetime
                          -> request_lifetime, subscriber_delivery
server -> client_connections, subscriber_delivery, storage_uring
```

Publication is an explicit server step: it passes each completed namespace effect to
subscriber delivery before freeing the request. Storage and write operations do not depend on connection
objects. Delivery sees a socket-independent `lb_peer` and uses reserve/wake
callbacks supplied by the transport. Its output remains shared across peers.
The server embeds that peer in a socket client and pins buffers through send CQEs.

Production code imports the implementation tools directly; there are no legacy
forwarding headers. `tests/cache_driver.h` is a test-only in-process driver used
by the storage/model tests and Datafriend's network tests. It exercises the same
operation state machines with an already-authorized `writable` flag. It is not a
network entry point and cannot be imported by production code. Shared container
primitives are included directly from `../containers/`.

The runtime is C99: structs, explicit lifetimes and error returns. `lb_task` owns
the batch, response and file buffers. The write tool prepares up to 16 independent
record writes concurrently per batch within the shared 64-operation storage pool.
Reads and synchronous callers retain the single-request path. Cancellation drains
submitted I/O before releasing buffers or writer ownership. No coroutines,
exceptions, STL containers or C++ runtime are used by the server.

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
The existing model tests also require a C++23 compiler; dependency checks and
transport regressions use Python 3:

```sh
python3 datafriend/tool/generate_attributes.py --check
make -C littlebear
littlebear/build/server --values /tmp/littlebear.values --port 7778
make -C littlebear test
make -C littlebear architecture
make -C littlebear sanitize
make -C littlebear debug
python3 scripts/test_littlebear_tcp.py littlebear/build/server
python3 scripts/test_littlebear_websocket.py littlebear/build/server
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

## WebSocket listener

Enable a second opening on the same server:

```sh
littlebear/build/server --values /tmp/littlebear.values \
  --bind 127.0.0.1 --port 7778 --ws-port 7779
```

TCP remains on port 7778; browsers connect to `ws://127.0.0.1:7779/`.
Omit `--ws-port` to keep WebSockets disabled. Both listeners use `--bind`;
port `0` asks the OS for an available port, printed at startup. They share the
same namespaces, subscriptions, authentication, storage, memory budget, and
io_uring loop. A native TCP writer can publish to a WebSocket subscriber and
vice versa. There is no separate bridge process or Datafriend Web dependency.

The WebSocket layer implements the [RFC 6455](https://www.rfc-editor.org/rfc/rfc6455)
HTTP/1.1 upgrade and binary framing. It removes client masks and supplies the
existing LBS5 parser with the unchanged stream:

```text
WebSocket binary payload → u32 big-endian length + LBS5 request → existing dispatch
existing reply/update → u32 big-endian length + envelope → WebSocket binary frame
```

Keep the four-byte Littlebear length prefix inside each WebSocket payload.
Input packets may span frames or messages, and a message may contain multiple
packets. Continuations and control frames may interrupt the stream. Each outgoing
Littlebear packet is one final binary WebSocket frame, including its length prefix.
The optional, case-sensitive subprotocol is `binary`; a browser may also omit it.
Any request path is accepted. Text messages are rejected with close code 1003;
compression and other extensions are not negotiated.

The browser-side transport adapter can use the standard API:

```js
function connectLittlebear(url, receivePacket) {
  const socket = new WebSocket(url, "binary");
  socket.binaryType = "arraybuffer";
  socket.addEventListener("message", ({ data }) => receivePacket(new Uint8Array(data)));
  return socket; // Wait for "open", then send an encoded, length-prefixed LBS5 packet.
}
```

The transport does not change how applications encode records, sign writes, or
reconcile state. The regular owner challenge/signature checks apply on both ports.
Origins are not restricted, and namespace reads remain public as on TCP; HTTP
cookies are not used for authorization.

Handshake headers are limited to 8 KiB. Frame payloads are limited to
`LB_MAX_FRAME + 4` (8 MiB plus the native length prefix); native packets retain
all existing validation and limits. Fragmented messages stream without accumulating
a second full packet. Each WebSocket connection adds about 8.5 KiB of accounted
transport state. Outgoing shared subscription bodies remain shared; `sendmsg`
adds only a connection-owned WebSocket header. Backpressure, slow-subscriber
fallback, cancellation, and the existing 30-second inactivity timeout also apply.
Use LBS heartbeats within that timeout for idle browser connections. WebSocket
ping/pong and close handshakes are handled by the transport itself.

The listener serves plain `ws://`. For HTTPS pages, terminate `wss://` at a reverse
proxy and forward HTTP/1.1 Upgrade requests to the loopback WebSocket listener.
See [deployment notes](../deploy/README.md#enabling-websockets). TLS is not added
to Littlebear and there are no new build dependencies.

The WebSocket regression starts isolated servers and checks both directions of
TCP/WebSocket record transfer, cross-transport replay rejection, fragmented and
coalesced input, invalid handshakes/frames, 16/64-bit frame lengths, shutdown, and
shared delivery to slow subscribers. It also runs the existing full protocol
regression through a WebSocket socket adapter. Run it against `server-debug` for
ASan/UBSan coverage. Its Python client uses the standard library; signed-record
and delivery checks use the same libsodium/xxhash helpers as the TCP tests.

## Channel storage

Each connection grows a dense channel array on demand. A shared `ct_map` maps
opaque nonzero 32-bit wire IDs to array positions; sparse IDs do not allocate
sparse storage. Active work holds an index across I/O, not a pointer into a
relocatable array. Unsubscribe removes the mapping, swaps the final entry into
the hole and repairs its index. Disconnect frees the array and map.
Subscription delivery rotates its starting position. The fixed 64-channel cap
is gone; allocation and container/protocol integer ranges remain the bounds.

## Connections and scheduling

Socket connections use stable 128-slot slabs and a free list; there is no fixed
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
The server emits one notification after the operation releases the write lock: successful
changes carry a borrowed `lb_publication`, while aborts, restarts and unchanged
revisions pass `NULL`. The transport either queues that publication or marks
subscribers for reconciliation, without walking the subscriber list twice.
Successful network op6 uploads complete without allocating a reply buffer; rejection
and reconciliation responses remain encoded. The synchronous C API retains its existing success replies.

Each connection may retain at most 16 MiB of logical delivery backlog, including
queued references and its active shared send. If a subscription cannot queue the
next update because that limit or the global allocation budget is reached, only
that subscription drops its unsent chain and receives a fresh reconciliation
opening after existing socket output drains. A revision gap does the same.
Other subscribers keep their queued messages. Fresh subscription/reconciliation
replies supersede older queued messages; channel close and disconnect release them.

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
caller's I/O. The network server uses the asynchronous queue for storage tasks.

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
