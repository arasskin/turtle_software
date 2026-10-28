# Datafriend native C interface

Include `datafriend/db.h` for storage, values, edits and watches; include
`datafriend/network.h` for synchronization. The implementation is C99. The older
`df_call` binary interface and C++ adapters remain available for existing callers.
Native apps no longer need to construct operation packets or attribute names.

## Store and values

```c
df_buffer error = {0};
df_options options = {
    .directory = "./tasks",
    .namespace_name = "local-tasks",
    .durability = DF_ASYNC,
    .map_bytes = 1u << 28,
};
df_store *db = df_store_open(&options, &error);
/* On failure, error contains owned UTF-8 bytes. */
df_free(error.data);
```

Options explicitly select buffered, synchronous or asynchronous durability. A
zero map size selects 1 GiB of virtual address space, not an immediate RAM
allocation. One store represents one namespace. Network namespaces are public
key hex strings; local stores may have ordinary names.

An entity ID and an attribute ID are both uint32. Together they identify one
value. `schema.h` generates `DF_ATTR_*` constants from the existing schema; this
change does not change schema IDs, fingerprints or stored data.

`df_int`, `df_bool`, `df_real`, `df_entity`, `df_string`, `df_string_z` and
`df_bytes_value` construct typed values. `df_encoded` accepts opaque already
encoded bytes, including application CRDT payloads. Typed encodings are unchanged.
Strings carry an explicit length; the `_z` helper computes it from a C string.
NaN/infinity are rejected and negative zero is normalized. Constructors borrow
input; appending a value to an edit copies it. Constants in a query are copied
before subscription returns.

`df_store_get` and `df_view_get` return a borrowed `df_slice` and `DF_OK`,
`DF_ABSENT` or `DF_ERROR`. Decode it using `df_value_int`, `df_value_bool`,
`df_value_real`, `df_value_entity`, `df_value_text` or `df_value_bytes`.
Borrowed values expire at mutation or release. Copy them if needed beyond that
point. Errors from native store operations are available through
`df_store_error`; open/close retain owned error buffers for use after store release.

## Atomic edits

```c
df_edit edit = {0};
uint32_t task = df_edit_create(&edit, db);
df_edit_set(&edit, task, DF_ATTR_KIND, df_string_z("task"));
df_edit_set(&edit, task, DF_ATTR_TITLE, df_string_z("Take a walk"));
df_edit_set(&edit, task, DF_ATTR_DONE, df_bool(false));
df_edit_set(&edit, task, DF_ATTR_ORDER, df_int(0));
df_status status = df_edit_apply(db, &edit);
df_edit_free(&edit);
```

Check the apply status; a failed append marks the whole batch failed. No subset
is committed. A batch processes operations in append order:

- `df_edit_set`: insert or replace a field.
- `df_edit_unset`: remove a field regardless of its previous value.
- `df_edit_retract`: remove only when its value equals the expected value.
- `df_edit_delete`: remove all fields on an entity.

Creation reserves IDs within the batch. The counter and fields commit in the
same transaction. Another intervening allocation invalidates that batch rather
than silently reusing its IDs. A discarded or failed batch does not consume IDs;
a successful creation with no fields still persists its reservation.

Apply and clear retain the builder's capacity, including after failure. Free
releases it. A reusable builder can be kept in an application model across edits.
The demo's short-lived edit wrappers free their builders after each action.

## Queries and watches

```c
static void changed(void *context, df_view *view) {
    /* Read the new result and update application caches. */
}

df_match clauses[] = {
    {df_var(0), DF_ATTR_KIND, df_const(df_string_z("task")), false},
    {df_var(0), DF_ATTR_TITLE, df_var(1), true},
    {df_var(0), DF_ATTR_DONE, df_var(2), true},
};
df_query_spec query = {clauses, 3, false};
df_watch *watch = df_watch_query(db, query, changed, context);
/* ... */
df_watch_remove(db, &watch);
```

Clauses constrain entity/attribute/value facts. Constants filter; repeated
variables join. `project` selects the facts retained in the result. The existing
limits of 32 clauses and 64 variables remain. An all-facts query is
`(df_query_spec){NULL, 0, true}`. Native queries use numeric attributes and a
normalized key for sharing, then compile directly without decoding a request.
Identical native queries share live results. Legacy packet queries retain their
own query-key representation and do not share results with native queries.

A watch calls its callback immediately with the initial result, then after a
commit actually changes its result. Local edits and remote network imports use
the same dispatch. Results are ready before callbacks run. Callback context must
outlive the watch. Callbacks may read results, add/remove watches, and edit other
stores. They must not edit or close the publishing store, destroy the network,
or recursively pump it. New watches receive their initial callback; they are
not also notified for the publication already in progress. Removing another
watch skips its pending callback. Failed edits conservatively notify reset so
application caches can recover; a poisoned store requires reopening.

`df_watch_entity` is shorthand for a fixed entity with required projected fields:
all requested fields must exist for it to match. `df_query_once` returns an owned,
detached result. It shares immutable values, but its map stays unchanged even if
an identical live query exists. Release it with `df_view_release`.

A watch owns its result. `df_view_retain` allows a result to outlive its watch or
store; after the last live subscription is removed it keeps its final contents.

`df_view_entities` fills a reusable `df_entities` buffer with distinct entity IDs,
optionally ordered by two int64 attributes and then ID. Missing sort fields sort
last. Keep the scratch object to reuse its capacity. Datafriend reuses its output and sort-key storage when capacity is sufficient;
sorting itself uses the platform `qsort`. Call `df_entities_free` at teardown.
The general change-summary compatibility API remains `df_result_changes`.

## Networking and lifetime

```c
df_network *connection = df_network_open("147.182.181.182", 7777, &error);
uint32_t owner_channel = df_network_push(connection, own_store, identity, &error);
uint32_t friend_channel = df_network_follow(connection, friend_store, &error);
/* From the application's serialized loop: */
df_network_pump(connection, 1500);
/* Outside pump/callbacks, when this friend is no longer needed: */
df_network_detach(connection, friend_channel, &error);
```

These three operations are functions, not callbacks:

- **Push** attaches the owner's authoritative store. Revisions schedule uploads.
- **Follow** subscribes to the namespace already recorded in a friend's store.
  Remote changes publish that store's query watches automatically.
- **Pump** progresses connection, reads, writes, reconciliation and notification
  on the calling thread. Its budget is soft between operations.

For event-driven integration, call `df_network_prepare(connection)` after pumping
or changing connection/store state. It returns a borrowed socket, read/write
interest, and a relative timeout in microseconds. Wait for either readiness or
the timeout, pump on the same owning thread, then refresh the watch. A zero
timeout means work remains (including complete messages buffered after the pump
budget expires); `UINT64_MAX` means no timer. Local writes, attach/detach, and
online changes must also wake that thread. Datafriend does not create a thread
or install platform callbacks. Polling `df_network_pump` remains supported.

The compatibility `df_network_subscribe` accepts an explicit public key and one
optional publication callback `(context, changed_view_ids, count)`. It runs after
remote commit and native watch dispatch. Ordinary native applications use follow
and watches instead of forwarding those IDs themselves. There are no separate
push/subscribe/pump callbacks. Connection state/errors remain polled getters.

Channel IDs are stable, monotonic and never reused during a connection object's
lifetime. The channel array is dense; removing an entry moves its last entry and
repairs ID lookup. Detach queues unsubscribe behind existing traffic, cancels the
local exchange, and releases the store immediately. Replies for retired IDs are
ignored. It preserves other channels and the socket; removing the last channel
closes the socket. Failed detach leaves the attachment intact. Detach must run
outside pump/callbacks. New attachments are allowed inside publication callbacks.

Only one network may attach a given store. Stores and signer identities must
outlive their attachment. Close the network or detach first, then close stores
and identities. `df_store_close` rejects an attached store or a close from its
callback; otherwise it flushes and consumes the store even on flush failure.

Authority and cache semantics are unchanged: owner data restores the server;
an absent server namespace preserves a friend's last state, while an existing
empty namespace reconciles to empty. This API adds no new network round trips.
