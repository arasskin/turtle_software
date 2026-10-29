# Common ground

A multiplayer drawing canvas using Janet and **native Datafriend**, synchronized
through Littlebear. Each native host has an independent local database and signing
identity. Browsers only handle input, rendering, and a local retry cache. There is
no Datafriend Web, Emscripten, WASM, npm, or hosted browser backend.

Strokes, unfinished strokes, cursor positions, and presence are all records.
Overlapping strokes retain separate IDs; no participant replaces the entire canvas.

## Start one native host

From the repository root:

```sh
./examples/canvas/run
```

The first launch builds the bundled Janet runtime, native Datafriend, and the
small C adapter. It needs CMake, Make, and a C99 compiler and downloads nothing.
Subsequent launches reuse the build. Open the complete invite URL printed by the
host. The default remote endpoint is **147.182.181.182:7777**.

A host publishes its own namespace. To receive another host's drawings, add its
public key to `--peers`. Littlebear does not provide room discovery or a shared
multi-writer namespace, so this example uses an explicit peer list. Each host
must follow the others. Once configured, the peer list and identity alias are
remembered in that host's data directory.

Two browser tabs connected to the **same** native host test local browser fanout.
For a real Littlebear test, use the two-host setup below.

## Two independent local hosts, one external Littlebear

Get the native public keys first. These commands create or reopen persistent
Datafriend identities in macOS Keychain; public keys are not secrets.

```sh
./examples/canvas/run --print-key turtle-canvas-alice
./examples/canvas/run --print-key turtle-canvas-bob
```

In terminal A, replace `BOB_PUBLIC_KEY` with Bob's printed key:

```sh
./examples/canvas/run --port 8787 --data build/data \
  --identity turtle-canvas-alice --peers BOB_PUBLIC_KEY
```

In terminal B, replace `ALICE_PUBLIC_KEY` with Alice's printed key:

```sh
./examples/canvas/run --port 8788 --data build/bob \
  --identity turtle-canvas-bob --peers ALICE_PUBLIC_KEY
```

Open both printed invite URLs. Draw in each: updates travel from its browser to
its local Janet/Datafriend host, across the internet to Littlebear, back into the
other native database, then into the other browser. The toolbar status indicates
whether the native Littlebear connection is online. Test with two browser windows
to watch cursors move; editing the display names makes participants easy to identify.

The same commands work on separate Macs. The browser remains connected to its
own local host; only native Datafriend connects to the external server. Use
different identity aliases and data directories for different native writers.
Never run two independent writable databases under the same identity: the owner's
namespace is authoritative and one database could replace the other's state.

Multiple peers are comma-separated: `--peers KEY_A,KEY_B`. Up to 32 peer namespaces
are supported by the example. `--peers ""` clears a remembered peer list.

## Persistence and offline testing

Paths are relative to `examples/canvas/` unless absolute. Defaults live in ignored
`build/data/`; choose an absolute `--data` path to keep drawings beyond build cleanup.
The directory's parent must exist.

| Path inside each data directory | Contents |
| --- | --- |
| `native-host.json` | Identity alias, public key, followed peer keys, last configured endpoint |
| `owner/` | This host's authoritative native Datafriend database |
| `replicas/KEY/` | A persistent, read-only native replica for that peer |
| `invite-key` | Access key for browsers connecting to this local host |
| `store/` | Original local-only example database, if one existed |

The first synchronized start copies original local-only drawing records into the
new owner's database. It preserves record IDs, drawing order, and the browser
invite key, and leaves the original database intact. Stop the old host before
migration so its database lock can be acquired. Migration is resumable and marked
with `migrated-native.json` after completion.

To test local edits while Littlebear is unavailable, start a previously configured
host with `--offline true`. It still serves its browser and commits local records.
Restart it without that flag (or with `--offline false`) to reconnect. Both sides'
independent strokes should converge, including overlapping strokes and hidden
records from **Undo mine**. Existing replica data remains visible while offline.

`--sync IP:PORT` selects another numeric IPv4 endpoint. `--sync off` runs the
original local-only example against `store/`, without Keychain or Littlebear; it
does not switch the synchronized owner database offline. Use `--offline true` for
that. Persistent signing identities currently require Datafriend's Apple or Android
secure-storage backend; this desktop launcher is tested on macOS. Linux can run
the local-only mode and Littlebear, but Datafriend currently has no Linux persistent
identity backend.

## Record flow and ownership

```text
Browser A → Janet A → native owner database A
                         ↓ signed TCP writes
                 external Littlebear
                         ↓ native reconciliation / updates
Browser B ← Janet B ← native replica of A

Browser B writes through its own owner database B, with the reverse path to A.
```

`store.c` binds Datafriend's store, identity, native network driver, and live query
watches to Janet. Each logical record is an entity under the existing
`document/state` attribute. `sync.janet` attaches the owner for signed push and
the peer stores for follow, then pumps the native state machine on Janet's event
loop. Remote imports trigger the same native watch API as local edits. Received
records stay in their replica stores and are never republished as local authority.

`room.janet` validates records and merges the projections. A stroke has a stable
ID, author, native writer public key, monotonically increasing version, color,
width, points, completion flag, hidden flag, and ordering number. Local creation
advances a Lamport ordering counter; concurrent equal counters are ordered by
record ID in every browser. Updates preserve the original order. An undo changes
the author's local stroke record to hidden; it cannot mutate a peer's namespace.

Cursor records contain position, color, name, active/online flags, version,
native writer, and timestamp. Disconnect updates the local cursor to offline.
Restart only marks that host's old presence offline. Movement and in-progress
stroke records update roughly every 55 ms; idle connected pages refresh their
cursor record every four seconds. Browsers hide presence older than 16 seconds.

The browser renders immediately and caches stroke records and pending versions
through its built-in IndexedDB API. That cache is a projection/outbox, not a
Datafriend implementation. Cursor records are not replayed from a previous page.
The app reports unavailable browser storage. HTML/JS still require the local host
to load; no service worker is installed for offline cold starts.

The native store uses synchronous durability before local publication. The UI's
“Saved locally” refers to this commit. Network `settled` does not mean an acknowledged
remote durable write; Littlebear is a disposable cache restored by native owners.
Preserve the owner databases and Keychain identities. Peer removal, room discovery,
key transfer/recovery, and database compaction are not implemented in this example.

Littlebear has public namespace reads and signed owner writes over plain TCP.
The local browser invite protects access to that local host; it does not encrypt
drawings on the external service. Display names/browser author IDs are not accounts.

## Browser sharing and limits

The host binds to loopback by default. Use `--host 0.0.0.0` to let browsers on your
trusted LAN connect, replacing localhost in the printed invite with this host's
LAN address. This still connects those browsers to one native database. The
**Invite someone** button shares this browser URL, not a native peer configuration.

The example supports 64 browser connections, 20,000 projected records, and 500,000
stored points. Long gestures split at 1,024 points into adjoining stroke records.
Limits include hidden strokes and old cursors. Source assets use no external fonts,
packages, or image requests.

## Tests and deployment

```sh
./examples/canvas/test             # Native build, model, framing, storage, local TCP
./examples/canvas/test --external  # Also two independent hosts through the live relay
```

Tests additionally require Node.js 18+ and Python 3.9+. External tests create
dedicated test identities in Keychain and disposable local databases, and publish
synthetic records into their own namespaces. They check overlapping writers,
incremental strokes, cursors, ownership, undo propagation, offline local edits,
restart/reconnect, and converged persistent replicas. They do not touch your demo
databases. The test identities remain in Keychain after test data is removed.

For a different test endpoint:

```sh
cd examples/canvas
python3 tests/external_sync.py --endpoint 127.0.0.1:7778
```

See [external Littlebear deployment](../../deploy/README.md) for the source build,
release packaging, health checks, and rollback procedure used for the live server.
