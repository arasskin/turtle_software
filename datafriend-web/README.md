# Datafriend Web

Copied from DrawDo Datafriend commit `73c4b9c5bb4e198599c9b65366fd400253eb0954`.
Original hashes and dependent working-tree snapshots are recorded in
[`vendor-provenance.json`](vendor-provenance.json). Original notices and
licenses remain intact. [Upstream README](UPSTREAM-README.md) describes the native
engine; its DrawDo documentation/application links refer to that original workspace.

`protocol/` contains only the client codec and reconciliation headers (plus C++
adapters) from Littlebear. There is no bundled server. These headers retain their
original bytes and recorded hashes; client includes now use this location.

## Browser build

Activate Emscripten (tested with 6.0.13), then from the Turtle Software root:

```sh
sh datafriend-web/browser/build.sh
node datafriend-web/browser/smoke.test.mjs
```

The output is `datafriend-web/build/browser/{datafriend.mjs,datafriend.wasm,client.mjs}`.
`EMCC=/path/to/emcc` selects a compiler. Set `DATAFRIEND_WEB_OUTPUT` to an absolute directory to build into an application’s static assets instead. The tests use the default output. No installed native Datafriend library is
used by the browser: the actual C database and sync state machine run in WASM.

```js
import {createSession} from '/datafriend/client.mjs';
const session = await createSession({url: 'ws://localhost:7778'});
const owner = session.owner(); // New ephemeral Ed25519 identity
owner.write({x: 10, y: 20});
const replica = session.follow(otherOwnerPublicKey);
const unsubscribe = replica.subscribe(state => console.log(state));
// When done:
unsubscribe();
session.close();
```

Omit `url` for local-only stores. `owner.key` is the namespace public key.
Replica wrappers reject local writes. Store callbacks publish locally immediately
and after network imports. A 16 ms pump serializes native network/database work.
The unchanged native protocol handles signed writes, optimistic deltas,
reconciliation, and reconnects. WebSockets carry its existing length-prefixed bytes
through the fixed-upstream bridge to Littlebear.

## Changes from the native engine

- `browser/bridge.c`: small exported state-document and network interface.
- `browser/client.mjs`: session/store lifetimes, subscriptions, JSON boundary, pump.
- `browser/flush.c`: no asynchronous disk thread; browser stores are buffered.
- `src/engine.c`: Emscripten-only `MDB_NOLOCK | MDB_WRITEMAP`. One JS context owns
  each store; MEMFS mappings are copies, so commits must update the mapped memory.
- `browser/build.sh`: compiles the existing engine, AELMDB, and Monocypher with
  Emscripten. Native `CMakeLists.txt` remains usable.
- The schema generator no longer writes the retired external Dart application.

No protocol or schema fingerprint change was needed. The first adapter stores
JSON under the existing `document/state` attribute. Documents are limited to 60 KB.
Keys are generated with the runtime's secure randomness and signing remains in C.
They are intentionally ephemeral: there is no Keychain equivalent or IndexedDB/
OPFS persistence in this first browser version. MEMFS files and keys are discarded
with the session/page. A WebSocket bridge is required because browsers cannot open
Littlebear's raw TCP endpoint directly.

`browser/network.test.mjs` tests two WASM sessions against real Littlebear: host
publication, client output, host update, and repeated optimistic updates.
The default native suite covers client and storage behavior. The upstream
`tests/network_test.cpp` embeds a server and is optional: it requires an explicitly
supplied external `DATAFRIEND_LITTLEBEAR_SOURCE` CMake path. Normal Dinopolis sync
checks use the deployed server via `browser/network.test.mjs`.
Upstream C tests remain in `tests/`; `tests/network_integration.dart` is an unused
historical file, not part of this fork's test commands.
