# Datafriend native engine and interface

For new C applications, use the [direct C API](datafriend-c-api.md). The binary
contract below is retained for compatibility and historical reference.

> The engine is now C99, with direct C APIs described in
> [Datafriend](../datafriend/README.md). The wire contract below remains supported;
> references to Dart describe the archived integration, which is unsupported.

The current architecture is described in [live mutable query results](datafriend-live-results.md).
The engine uses packed-key dense maps for query results and direct namespace
triple edits. Persistent query trees and local view reconciliation are removed.
Entity-local queries now [reevaluate only changed entities](datafriend-incremental-queries.md)
after edits/imports; initial subscriptions, recovery, joins and all-facts queries
retain full evaluation. The C/Dart interface is unchanged by this optimization.
The [grouped query planner](datafriend-query-planner.md) gathers same-entity
fields together, chooses access paths using current index counts, and avoids
unused value bindings. General joins recurse between entity groups.

## C ABI

Storage is now [attribute-level AELMDB v3](datafriend-fact-storage.md), with a
generated shared attribute schema. `df_attribute_schema(reply)` returns its
UTF-8 fingerprint in an owned buffer; Dart verifies it before opening a store.

`df_open_namespace`, `df_open`, `df_close`, `df_alloc`, `df_free` retain their
ownership rules. Calls are synchronous, non-leaf FFI; no native-to-Dart callbacks.
`df_call` takes version byte 1 followed by an operation byte. Integers are big-endian;
`blob` is u32 length followed by bytes. Triple encoding is u64 entity, blob attribute,
blob value. Terms are u8(0)+blob constant or u8(1)+u8 variable number.

The Dart query API accepts `(QueryClause, bool include)` pairs and derives the
wire projection indices below internally. Entity-position integer IDs and
`Entity(id)` both encode as references; value-position integers remain numbers.

| Op | Request after version/op | Reply |
| --- | --- | --- |
| 1 | u8 all, u32 clause count, [term entity, blob attribute, term value], u32 projection count, [u32 clause index] | u64 subscription ID |
| 2 | u64 subscription ID | u32 count, [triple] |
| 3 | u64 subscription ID, u32 remove count, [triple], u32 add count, [triple] | changed subscriptions |
| 4 | u64 subscription ID | empty |
| 5 | empty | reserved u64 entity ID |
| 6 | empty | u32 count, [u64 entity, blob envelope] |
| 7 | u32 count, [u64 entity, u8 present, optional blob envelope] | changed subscriptions |
| 8 | u32 batch count, [u64 subscription ID, u32 remove count, [triple], u32 add count, [triple]] | changed subscriptions |
| 9 | u64 subscription ID, u64 entity, blob attribute | u8 present, optional triple |
| 10 | u32 remove count, [triple], u32 add count, [triple] | changed subscriptions |
| 11 | none | blocking disk flush; empty reply |
| 12 | none | u32 count, [u64 packed EA key, blob fact record] |
| 13 | u32 count, [u64 packed EA key, u8 present, optional blob fact record] | changed subscriptions |

Changed subscriptions are u32 count followed by u64 IDs. Op 10 is the production
edit interface. Ops 3/8 are compatibility conveniences that validate handles and
apply direct database edits; they do not reconcile views. All reply/error buffers
must be freed. This prototype changed its ABI; rebuild Dart and native together.

Open mode 0/1/2 selects buffered/synchronous/asynchronous flushing. The production
Dart default is mode 2. `df_close_checked(engine, error)` flushes and releases the
engine even if it returns an error; `df_close` remains the no-throw finalizer.

`Datafriend.deleteEntity(id)` sends an op-7 absent whole record. This atomically
removes all of the entity's fields and publishes affected results before returning,
without reading or re-encoding those fields in Dart. A missing entity is a no-op.

`df_result(engine, subscription, error)` returns an owned shared live result handle.
`df_result_info` returns u64 count; `df_result_list` returns u32 count plus triples;
`df_result_same` compares handle identity, not content versions. Release with
`df_result_release`. The old snapshot ABI is removed. Explicit close plus Dart
native finalizers manage lifetime. Retained handles observe edits in place; after
final unsubscribe or engine close they retain their final contents.

The caller serializes reads and blocking edits on one application thread. No map
read locks or concurrent-reader contract. Engine calls execute directly on the
caller; there is no engine queue or private owner thread. Query callbacks run in Dart, from the returned changed
IDs, before edit returns. `subscribe(query, onChanged: callback)` needs no polling.
Inside that callback, `view.changes` provides owned sets of changed result entity
IDs and projected attribute names, plus `membershipChanged` and `reset` flags.
The summary reflects actual map writes/removals, including results affected by
joins. Read it immediately in the callback; native summaries are not an event log.
Failed Dart edit/import calls conservatively notify every subscription with
`reset: true`, run the publication hook, then rethrow. Callbacks must handle a
reset by dropping caches; poisoned storage still requires reopening.

## Direct map reads

The production app uses [direct reads](datafriend-direct-reads.md), not triple export.
New ABI functions are additive; older Dart binaries can use the new native
library. The selective-cache Dart adapter requires a native rebuild that exports
`df_result_changes`.

- `df_result_attribute`: name bytes → shared schema ID (zero if undeclared).
- `df_result_value`: numeric entity/attribute → borrowed value bytes, no allocation
  or serialization. Status 0 means present, 1 absent, -1 unavailable. Never free
  this buffer; consume it before any edit or close.
- `df_result_entities`: owned host-endian uint32 ID array, optionally sorted by
  signed int64 order/secondary fields and entity ID. Its buffer is freed normally.
- `df_result_find`: first matching entity, -1 absent, -2 unavailable.
- `df_result_attributes`: one projected entity's attribute names, encoded as
  `u32(count), [blob(name)]`. Owned buffer; no values or other rows copied.
  Missing entities produce zero names. Exposed as `result.attributes(entity)`.
- `df_result_count_value`: matching entity count, -1 unavailable.
- `df_result_changes`: owned summary encoded as `u8(reset), u8(membership),
  u32(entity_count), [u32(entity)], u32(attribute_count), [blob(attribute_name)]`.
  Sets have unspecified order. Initial/rebuilt native results request a reset.
  Each affected query clears/accumulates its summary during an edit. Read only
  when processing its corresponding notification; unchanged reevaluations can
  clear the previous summary. Status 0 is success, 1 error.

Dart resolves/caches only attribute metadata, consumes borrowed values immediately,
and uses a native finalizer for owned ID buffers. Strings and byte values returned
to application code own their data. Bulk `df_result_list` remains an explicit
compatibility/export operation; it is no longer the production read path.

## Canonical representation and server boundary

All integers in the Datafriend codec are big-endian. A `blob` is a `u32` byte
length followed by those bytes. Hashes are **XXH64, seed 0**, combined by XOR.

| Item | Representation |
| --- | --- |
| Entity ID | Namespace-local uint32 |
| Attribute ID | Shared, explicitly assigned uint32 enum value |
| AELMDB/server key | `u64((entity << 32) | attribute)` |
| Query result | Dense packed entity/attribute map → shared boxed value |
| Box hash | `XXH64(value_bytes)`; computed once per immutable box |
| Fact fingerprint | `XXH64(u64(packed_key) + value_bytes)` |
| AELMDB value / fact envelope | `u64(fact_fingerprint) + value_bytes` |

Attribute names resolve through the generated shared schema; undeclared names
are rejected in writes and queries. Each encoded value may contain at most
**65,527 bytes**, plus the eight-byte fingerprint prefix. That prefix is local
envelope metadata: a future server driver supplies it separately from value bytes
when writing LittleBear. Fact imports validate keys and fingerprints before
publishing changes. Whole-entity import/export helpers remain document utilities;
ordinary edits do not encode or decode whole entities. The v3 format starts fresh
and rejects earlier codecs; there is no migration layer.

C++ treats values as immutable boxed opaque bytes, including small scalars. Dart uses these scalar tags:
`0=null`, `1=bool + u8`, `2=string + blob(UTF-8)`, `3=int64 bits`,
`4=float64 IEEE bits`, `5=bytes + blob`, `6=entity-reference + u64`.
Floats must be finite; negative zero is normalized on encoding. Byte values are
copied so callers cannot mutate stored triples. Entity references retain their
u64 boundary encoding, but IDs outside 0..2^32−1 are rejected. Use `Entity`,
not scalar int, for joins. Scalar int values still cover signed int64.

AELMDB is pinned at `359b89807d638e0f255e012cfe8c64dca33a0c02`, compiled with
`MDB_HASH_SIZE=8`, XOR add/subtract, and format tag `0xDFA1`. Its metadata also
checks the codec string and hash offset on reopening. Default writes use
[asynchronous disk flushing](datafriend-async-flush.md); edits commit and publish
before the background flush. `flush()` and checked close provide blocking durability
barriers. `durable:true` retains synchronous flushes and `durable:false` uses
`MDB_NOSYNC` without automatic flushing for experiments only. The initial
map size is 1 GiB; exhaustion returns an error with the previous state intact.
One process owns a store directory using an exclusive file lock.

`export_facts` returns a consistent packed-key/envelope batch. `import_facts`
applies authoritative fact replacements/deletions atomically and updates affected
views. These are transport integration hooks, **not a connection manager**.
`export_entities` and `import_entities` retain whole-document utility semantics.
Server authentication, authority/namespace status, framing, record exchange,
retry scheduling and background lifecycle await the separate server integration.
Do not interpret an empty server cache as an instruction to delete owner data.
The 64-bit hash/XOR collision tradeoff is deliberate; this is not a cryptographic
integrity or authentication mechanism.


## Validation and remaining integration

The tests cover joins, repeated projections, shared subscriptions, live handle
lifetime, stale retractions, edits without any subscriptions, transaction failure,
and recovery after maps have already changed. The app retains lazy projections
and the existing Flutter ChangeNotifier/ListenableBuilder bridge.

Network transport remains separate work. Native
background imports must be delivered through the same serialized app boundary.
See [progress](datafriend-progress.md) for completed checks.
