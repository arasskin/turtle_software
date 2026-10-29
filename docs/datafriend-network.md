# Datafriend ↔ Littlebear synchronization

The flat unordered packed-EA result map remains the production representation.
Entity ordering stays a separate cached UI representation. Network reconciliation
uses the existing AELMDB and Littlebear indexes, not the UI views.

## Supported native client

`df_network` owns nonblocking TCP framing, multiplexed channels,
reconciliation scheduling, signed writes, reconnect/backoff and heartbeats.
The application pumps it on the same serialized context as engine/view access.
Numeric IPv4 and IPv6 endpoints are supported; DNS resolution is not implemented.
No futures, network worker or engine locks are involved. The pump has a soft
budget between operations; one database apply or reconciliation is not preempted.

```c
#include <datafriend/network.h>

/* Stores are opened with their owner public key as namespace. */
df_network *connection = df_network_open("147.182.181.182", 7777, &error);
df_network_push(connection, owner, identity, &error);
uint32_t channel = df_network_follow(connection, replica, &error);
/* Replica query watches receive remote commits automatically. */
df_network_pump(connection, 1500);
/* When removing a friend, outside pump/callbacks: */
df_network_detach(connection, channel, &error);
```

See the [native C API guide](datafriend-c-api.md) for complete ownership,
notification and detach contracts. `push`, `follow` (subscribe), and `pump` are
functions. The compatibility subscription interface has one optional callback
with changed query IDs; ordinary native apps use store watches.

Stores and the signing identity must outlive their attachment. Detach or close
the connection before closing them. Each attached store
has one sync exchange; do not call its low-level `sync` functions separately.
Callbacks run on the calling thread after complete remote publication. They may
read views and edit other stores, but must not destroy those stores/the connection
or call `pump` recursively. `online(false)` disconnects and cancels snapshots;
`online(true)` resumes, repushes owners and reattaches replicas. Error and byte,
request, restart, signing, reconciliation and apply counters are exposed directly.
Network disconnection never rolls back a successful local edit.

Connected owners now send committed edit batches with the resulting full
namespace fingerprint in one signed message with no success reply. Owners do not
wait before queuing subsequent edits. `settled()` describes local output and
reconciliation state; it does not confirm remote delivery. Matching batches are forwarded
directly to subscribers, which check the fingerprint before committing and
publishing views. Negentropy handles mismatches, reconnects and bounded-journal
overflow. See [optimistic synchronization](datafriend-optimistic-sync.md).

Subscribers retain received deltas in order across pump calls. Each delta is
verified against the resulting database state before publication; a newer queued
fingerprint is not compared against an earlier, still-incomplete state. Pending
updates keep `settled()` false and make `prepare()` request another immediate
pump even after the socket is drained. Each channel's incoming queue is capped
at 16 MiB including queue nodes. On overflow, it retains the newest update;
fingerprint mismatch then falls back to reconciliation. A fresh reconciliation
opening or a changed server epoch replaces the obsolete queued chain.

The Sokol demo integrates this client on both phones. Its app-level sequence CRDT
uses one namespace per writer; see [document sync and measurements](datafriend-document-yrs.md).
The server permits public reads and authenticates owner writes. Transport remains
plain TCP: no transport encryption or authenticated server responses.

## Historical Dart client interface (unsupported)

The following describes the retired adapter from which the native state machine
was ported. It is retained as protocol history, not the supported application API.


```dart
// Datafriend loads or creates this owner's key in platform secure storage.
final identity = LittlebearIdentity.open('primary-owner');
final store = Datafriend.open(directory, namespace: identity.namespace);
final endpoint = LittlebearConfig(host: 'sync.example.org');
final push = store.sync.push(endpoint, identity: identity);

// Ordinary edits publish local views immediately and schedule an owner push.
store.edit(add: [Triple(task, Attribute.title, 'Updated')]);
await push.syncNow(); // Optional explicit convergence/error barrier.

final friend = Datafriend.open(friendDirectory, namespace: friendPublicKey);
final subscription = friend.sync.subscribe(endpoint);
```

`push` sends the owner's authoritative contents. `subscribe` registers a public
replica and leaves its channel open for server-initiated reconciliation. Both
share one persistent TCP connection per host/port within the Dart isolate, including
when there are several namespaces. Local query subscriptions still use
`store.subscribe(query)`; the network operations live under `store.sync`.

Replicas' local edits are permitted but overwritten by the next reconciliation;
this is not concurrent multi-writer merging. Use one owner per namespace. An
absent server namespace preserves the replica and sets `waitingForOwner`; the
subscription remains registered. An existing empty namespace removes replica
facts. Registering a subscription never creates a server namespace.

`disconnect()` detaches a namespace and releases its channel; the connection
closes when its last channel detaches. `close()` also cancels native reconciliation
before closing AELMDB. The old `connect(config)` and `Datafriend.open(network: ...)`
interfaces remain compatibility bridges: push mode calls `sync.push`, and pull
mode calls `sync.subscribe`. `pollInterval` is retained but no longer schedules
polling. `lastError`, `lastSynced`, `syncing`, exchange/restart counts, wire-byte
counts, and challenge/signed-batch/request counters are available on the returned
handle and `store.network`.

`LittlebearIdentity.open(alias)` creates or reopens an owner identity in
Datafriend's native secure store. Dart retains an opaque handle and the public
namespace; seed generation and signing happen in C++, with no seed import/export
API. Keep the identity alive while its connections use it, then call `close()` to
wipe its native signing key. Closing does not delete the stored identity. A push
requires an identity whose public key matches the namespace; pulls need only that
public namespace. `ephemeralForTesting()` explicitly creates an unpersisted native
identity for tests, without accessing any existing user's key.

Apple stores the seed as a non-synchronizing [Keychain item](https://developer.apple.com/documentation/security/ksecattraccessibleafterfirstunlockthisdeviceonly) with
`AfterFirstUnlockThisDeviceOnly`. Android stores AES-GCM encrypted seeds under
`Context.getNoBackupFilesDir()`, using a non-exportable [Android Keystore AES key](https://developer.android.com/privacy-and-security/keystore).
The Android platform adapter lives in Datafriend, not Dart; app startup installs
it with `IdentityStore.install(context)` before starting Flutter. The app Gradle
configuration includes its Java source and JNI keep rules. Corrupt ciphertext,
missing wrapping keys/records, unavailable secure storage and invalid aliases fail
the open operation; they never silently rotate a namespace. Android serializes
creation across threads/processes and atomically writes/verifies ciphertext.

These are protected-at-rest Ed25519 identities, not hardware-only Ed25519 signing.
Seeds are loaded into Datafriend memory for signing and owned secret buffers are
wiped on close. Platform APIs may make internal copies. Monocypher 4.0.3 supplies
portable RFC 8032 signing; native code restricts signing to this key's LBS5 write
envelope and current schema. There is no plaintext-file fallback. Persistent
identity storage is currently supported on Apple and Android; other hosts fail
closed, while explicit ephemeral test identities work on Linux. Key recovery,
export, cross-device transfer and rotation remain separate future features.

Network namespaces use 64 lowercase hex digits in the Dart API and 32 raw bytes
on the wire. Old named network namespaces are no longer accepted; local-only
stores may still use ordinary names. Record IDs and fingerprints remain 8 bytes.

The Dart part of Datafriend owns asynchronous `Socket` I/O, coalescing and
retry. Local edits only schedule a timer; they never wait for the network.
Scheduling does not depend on having query subscribers. Owner pushes run on
attachment, edits, explicit `syncNow()` and reconnect. Subscriptions receive the
first reconciliation packet directly from the server at registration and after
cache updates, without a separate change notification/request round trip.
Failures back off to 32 seconds; stale owner exchanges get up to three immediate
retries. Edits during a push schedule a trailing exchange. A connection heartbeat
every ten seconds detects broken idle connections without polling namespaces.
Reconnect reattaches subscriptions and repushes owners to restore a restarted cache.
`syncNow()` explicitly refreshes a subscription; an absent or currently-writing
namespace can return a waiting state while the channel awaits the next opening.

The transport is plain TCP, with no certificates, TLS or bearer tokens. Android
release manifests request INTERNET permission. Transport encryption and server
identity verification are not provided by this endpoint.

## Native consistency and publication

Each exchange opens an AELMDB read transaction in the engine's serialized calling context.
Range count/XOR, rank selection and bounded enumeration use AELMDB directly.
No full database export, copied index or persistent UI tree is built. Reader
slots belong to transactions (`MDB_NOTLS`) so this snapshot can coexist with
short-lived local read/write transactions. Snapshot pages remain pinned until
completion/cancellation; exchanges have a 30-second client deadline.

The shared `LBR3` reconciler compares independent uint64 packed-EA keys and their
stored uint64 fingerprints. A differing value under an existing key produces a
put. Remote-only keys become deletions for a pushing owner; local-only keys
become deletions for a pulling replica. Zero and maximum keys are representable.

Push records are paged at approximately 256 KiB. Pull requests fetch at most 64
records (under 4.2 MiB at the maximum record size). Pulled pages are validated and
staged natively; only a complete difference set enters one existing atomic local
transaction. Query indexes/results and Dart notifications use the ordinary
publication path. No background thread mutates a map while Dart reads it.

A local revision check rejects pulls if a local edit occurred during the exchange.
A server boot nonce and namespace revision accompany every request, including
record transfer. Intervening server writes or cache restarts invalidate the
exchange. No raw pointer into the relocatable namespace table is retained across
requests. Stale subscription replies carry a fresh server opening when the namespace is
stable. If a write is still underway, the subscriber waits for its completion
opening. A new opening invalidates any older in-flight exchange; fetched pages
from that exchange are discarded before local publication.

Local persistence remains asynchronously flushed. Littlebear is a disposable
cache, with its real shared slab-wide index and size-class value file. Its network
batch may be partially applied if server I/O fails; its revision is invalidated
before applying the first record, and a new owner reconciliation repairs the
cache. This is the previously accepted eventual remote coherence tradeoff; it
does not weaken the atomic local publication rule.

## Linux TCP server and owner authentication

`littlebear/server.c` uses one io_uring completion loop for accepts,
socket receives/sends, and file reads/writes. It requires Linux, C++23 and
liburing and libsodium, without OpenSSL. It is the default Littlebear build;
the old echo prototype has been removed.

```sh
make -C littlebear server
littlebear/build/server --values /path/to/values --bind 127.0.0.1 --port 7778
```

Only `--values` is required. The bind address and port above are the defaults;
`--port 0` selects a temporary port for tests. Certificate, key and credentials
options have been removed. The [service unit](../deploy/littlebear.service)
uses this same build, explicitly binding port 7777 on all interfaces to preserve
the deployment endpoint. Standalone defaults remain loopback:7778. Updating an
existing droplet requires reinstalling the unit; no live deployment was performed.

Reads and reconciliation are public. Begin-owner-sync requests carry the first
negentropy packet and never create a namespace. An absent namespace can be reconciled
as an empty source without allocating a namespace or revision entry. Readers
still receive absent status and retain their local copy.

Operation 0 compares the first owner offer against the current server state and
returns the server epoch and revision together with its reconciliation response.
Subsequent operation-1 rounds must match that version. The client pins its local
snapshot before sending the first offer and processes the response against that
same snapshot.

The server piggybacks a 32-byte challenge on the first differing reconciliation
response for a push, including operation 0. An absent namespace always receives
a challenge in the opening response, even when the owner is empty, so a signed
empty apply can establish its presence. There is no separate challenge operation
or authentication round trip. One challenge covers every write batch in the
exchange and subsequent optimistic writes, with its sequence starting at zero. Equal pushes to existing namespaces (including the small
initial-offer optimization) and all pulls skip the challenge. The `LBR3`
reconciliation engine and its packets are unchanged; this metadata belongs only
to the outer sync envelope.

Each apply request includes that challenge, the next uint64 sequence, and an
Ed25519 signature over the complete preceding request body: protocol/version,
request ID, channel ID, operation, public-key namespace, schema, boot nonce, revision, changes, challenge
and sequence. The server verifies against the full namespace public key. It
consumes the sequence before entering storage, even if a validly signed request
is stale or its storage operation fails. Reconnect/reconcile after a lost reply;
do not resend the old signed packet. A successful new sync start on the same channel invalidates the old challenge;
connection close and server restart discard all challenge state. Existing socket
deadlines bound idle connections. No persistent credentials or replay table is
required, and authentication adds 48 bytes per channel.

Only a verified apply can create or mutate a namespace. An empty signed apply
creates an existing empty namespace, distinct from absence. Unsigned, tampered,
out-of-order or replayed writes are rejected before storage. The former test-only
unauthenticated build has been removed; integration tests use the production
binary and real signatures.

The cache directory retains 128-bit internal keys derived with BLAKE2b-128 from
the full public key. Namespace authentication never uses the 8-byte record
fingerprints. Ed25519 verification uses [libsodium's signature API](https://doc.libsodium.org/public-key_cryptography/public-key_signatures);
Datafriend signs natively using [Monocypher's RFC 8032 Ed25519 API](https://monocypher.org/manual/ed25519).
These signatures authorize client writes. TCP remains unencrypted and server
responses are not authenticated by this protocol.

The driver has eight fixed connection slots, up to 64 namespace channels per
connection, and eight-MiB frames. It handles fragmented/coalesced frames and short
reads/writes. Socket activity refreshes a 30-second deadline. Receive and send
completions are independent, so idle subscribers can receive unsolicited openings.
There is one input buffer, one output buffer and one active storage request per
connection; updates coalesce by namespace revision. Cancellation completions are
drained before reusing a connection slot. Coroutine frames retain submitted
file-I/O buffers through disconnect/shutdown. No poll loop or worker pool is used.

Subscriber exchanges do not hold namespace-wide locks. Every record fetch checks
the revision before and after file I/O. Owner applies invalidate old exchanges
before changing records. A submitted kernel read pins that particular record
until its completion, so a conflicting write can require retry; it does not wait
for the subscriber's remaining reconciliation or pages. Writes remain serialized
while a batch is in flight. Datafriend serializes its owner pushes. The transport
is Linux-only; macOS client tests use a local Linux container.

Each reconciliation response is bounded to eight MiB and each accumulated
have/need set to one million keys. Exceeding these bounds rejects the exchange;
paging large **payloads** does not remove these reconciliation limits. Admission
budgets and performance under many concurrent namespaces remain operational
benchmark work, rather than claims established by the correctness tests.

## Envelope

All integers use network byte order. Each TCP message has a uint32 byte length.
Request body: `LBS5` magic (three ASCII bytes and byte 5), uint32 request ID,
uint32 channel ID, uint8 operation, length-prefixed public-key namespace/schema,
uint64 boot nonce, uint64 revision, length-prefixed payload. Request IDs are
nonzero; channel IDs are nonzero except for connection heartbeats. Channels stay
bound to their namespace until channel close/disconnect. Apply requests append 32
challenge bytes, a uint64 sequence and a 64-byte detached signature; other requests
have no auth trailer. The signed bytes run from `LBS5` through the sequence,
including both IDs and excluding the outer frame length and signature. The schema
is the generated shared attribute fingerprint. Older envelopes are rejected.

| Operation | Payload / behavior |
| --- | --- |
| 0 | Begin owner sync: first `LBR3` reconciliation packet. Returns the first comparison, current epoch/revision and status 0 (present) or 3 (absent). Resets authorization after a successful opening and includes a challenge if upload is needed. Never creates a namespace. |
| 1 | `continue_reconciliation`: `LBR3` packet against the supplied epoch/revision. Used by owners and readers. |
| 2 | `fetch_records`: uint32 count and uint64 keys. |
| 3 | `apply_records_at_revision`: signed, revision-checked apply; uint32 count, then key/presence/optional blob records. |
| 4 | Retired; rejected. Use operation 8 with delivery mode 0. |
| 5 | `close_channel`: empty payload; releases the channel, its subscription and owner authorization state. |
| 6 | `stream_record_changes`: signed streamed apply; uint64 resulting namespace fingerprint, followed by the ordinary put/delete batch. Reuses the connection challenge/sequence. Matching fingerprint is silent; mismatch emits unsolicited status 6 and requires a fresh start. |
| 7 | `heartbeat`: channel 0, empty namespace/payload. |
| 8 | `subscribe_to_records`: one delivery-mode byte, 0 for reconciliation notifications or 1 for record batches with reconciliation fallback. Both return an initial reconciliation opening or absent/busy status. Repeating the request changes the mode. |

The old one-byte operation-0 payload, operation 4, and empty operation-8 payload
are rejected. Deploy the updated server and
clients together; this change retains the envelope magic and operation IDs but
changes the operation-0 contract and operation-8 payload. Read-only clients begin
with operation 8 instead of operation 0.

Fact blobs retain
`XXH64(BE64(EA) || value)` followed by typed value bytes. Only signed apply can
create namespaces, including existing empty ones.

Responses carry uint32 request ID, uint32 channel ID, uint8 status/flags, uint64
boot nonce, uint64 revision, then payload: a 25-byte header. Request ID zero
identifies an unsolicited server event. Bit 7 of status marks 32 challenge
bytes inserted after the revision; the lower seven bits hold the status. The
challenge appears once per owner exchange, on an absent owner opening or the first
differing reconciliation reply (which may be operation 0). Reads and equal pushes to existing namespaces do not
require challenges.

Status 0 succeeds; 1 restarts the exchange (with a fresh opening when stable, or
empty payload while writing); 2 rejects; 3 means absent; 4 is an unsolicited
opening for an existing namespace; 5 is an unsolicited optimistic update with
uint64 resulting fingerprint followed by put/delete records. Status 5 is sent
only to operation-8 subscribers using delivery mode 1. Status 6 requests owner reconciliation after a
streamed-write mismatch; it has no payload. Later queued deltas are discarded
(with signature sequences still checked) until a successful new start. Rejected
streamed writes emit unsolicited status 2; clients reconnect. Subscriber opening payloads are ordinary `LBR3` initial
packets. Owner opening payloads are responses to the client's first `LBR3` packet,
preserving the small-set initial-offer optimization. The client processes
these in the difference-collecting role and requests values at the end; the server
remains stateless between reconciliation messages. Record fetch replies carry
count and key/blob pairs. The server validates apply syntax and fingerprints
before mutation and rejects duplicate keys.

The C ABI's existing version-1 `df_call` adds operation 14 (network command plus
blob) and 15 (stage/apply a pulled page, returning changed subscription IDs).
Network commands: 0 begin, 1 reconcile, 2 next push page, 3 next fetch page
(one-byte final-page flag then count/keys), 4 cancel, 5 begin from a server opening.
One exchange per store.
These are internal plumbing; application code uses `LittlebearConfig`.

## Validation

- Native integration covers real AELMDB plus real Littlebear storage, 8,001
  initial facts, same-key edits, deletions, namespace separation, snapshots,
  stale local/server versions, an owner write during an outstanding subscriber
  read, malformed batches, notification behavior and a
  paged transfer larger than eight MiB.
- `scripts/test_littlebear_tcp.py` tests the production Linux TCP server with real
  signatures: unsigned/wrong-owner/tampered/truncated requests, same-connection,
  cross-connection and restart replay, sequence ordering, challenge rotation,
  stale-write consumption, public reads, empty creation, namespace isolation,
  fragmented/coalesced frames, disconnects and pending-I/O shutdown. It also covers
  multiplexed subscription openings, stale-fetch restart packets and unsubscribe
  isolation.
- `scripts/test_datafriend_network.py --linux` runs Dart transport with the native signer against the
  production server in a one-CPU, 256-MiB container. It covers Unicode, edits and
  deletions, notifications, shared persistent connections, server-initiated updates,
  no subscriber polling, unsubscribe isolation, restart/resubscription/repopulation,
  automatic scheduling, close
  during I/O, no challenges for reads/equal pushes, signed empty creation and
  a 9.6-MB paged transfer using one challenge across all its batches. Request
  counts verify that pushes use only start, reconciliation and apply messages,
  with zero extra authentication round trips.
  A test proxy pauses a fetch, commits an owner update and then releases the old
  request; the replica publishes only the final state after restarting.
- Build `scripts/datafriend-network.Dockerfile`, compile `make -C littlebear
  server`, then set `DATAFRIEND_LINUX_IMAGE` and `DATAFRIEND_LINUX_SERVER`
  to the image and absolute Linux binary path. Build the native Datafriend library before running the Dart transport tests.
  The launcher publishes on host loopback only.
- Current subscription protocol results: [Linux integration](validation/datafriend-subscribe-linux.json)
  and [ASan/UBSan integration](validation/datafriend-subscribe-linux-sanitizer.json).
  Native network and identity tests also exercise the new envelope. Earlier TLS,
  LBS4/piggyback and identity interoperability reports are historical.

Simulation workload measurements are documented separately; native container or
query timings should not be presented as network latency or complete Flutter frames.

### Native identity validation

`identity_test` covers the RFC 8032 known-answer vector, independent native
handles, signature verification and rejection of wrong-owner, non-write,
truncated and trailing-data envelopes. It also passes Linux ASan/UBSan.
The existing Linux Littlebear integration now exercises native signing through
Dart transport, including paged writes; see the
[interoperability report](validation/datafriend-native-identity-network.json).

Run `flutter test integration_test/identity_test.dart -d <device>` from `src/app`
to test real platform storage. Android checks AES-GCM tampering and missing-record
failure, then restores ciphertext and verifies the same public identity. iOS
checks Keychain creation/reopening in the packaged app. For process restart,
build `--target tool/identity_probe.dart` and launch twice without uninstalling:
the second process checks the prior public namespace. Flutter's integration test
runner removes its test APK, so repeated test commands do not test that case.
Only public-key markers are written by either probe. See the
[device validation report](validation/datafriend-native-identity.json).
