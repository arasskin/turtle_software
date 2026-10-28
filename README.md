# Turtle Software

Building blocks for small applications that run on your computer with a browser interface.
Janet handles local work and serves the UI; Datafriend provides local state and queries;
Littlebear provides shared synchronization when an application needs it.

| Directory | Contents |
| --- | --- |
| `datafriend/` | Native C99 database, queries, identities, and sync client |
| `littlebear/` | Linux io_uring synchronization server |
| `datafriend-web/` | Separate Emscripten/browser fork, including its JavaScript API |
| `janet/` | Reusable HTTP, WebSocket, JSON, and tracing modules from Visualize |
| `containers/` | Shared header-only C containers |
| `third_party/` | Janet runtime source, pinned AELMDB, Monocypher, xxHash, and browser-fork test dependencies |
| `docs/` | Native API, protocol, storage, and reconciliation documentation |

These are the existing components brought together, not yet an application launcher
or distribution/update system. The native and browser database implementations remain
separate. Browser storage and identities are currently ephemeral; native Datafriend
provides persistent local storage. A general Janet-to-Datafriend binding is not yet
part of these utility modules.

## Build and check

Native Datafriend requires CMake, a C99 compiler, and a C++20 compiler for its tests:

```sh
make -C datafriend test
make -C containers test
```

Littlebear requires Linux, liburing, and libsodium for the server. Its portable tests
also run on macOS with libsodium installed and `CXX=clang++`:

```sh
make -C littlebear test
# Linux:
make -C littlebear server
```

Build the bundled Janet runtime and check the utilities:

```sh
make -C third_party/janet
third_party/janet/build/janet janet/tests/run.janet
VISUALIZE_TRACE=1 third_party/janet/build/janet janet/tests/run.janet
```

The browser fork requires Emscripten (originally tested with 6.0.13) and Node.js:

```sh
sh datafriend-web/browser/build.sh
node datafriend-web/browser/smoke.test.mjs
```

Builds go inside their component's ignored `build/` directory. See the component
READMEs for API details and live synchronization checks.

## Source migration

The initial source snapshot includes the working copies from DrawDo, Dinopolis,
and Visualize, including then-uncommitted changes. `SOURCE_PROVENANCE.json`
records base commits and pre-adaptation file hashes; it does not claim those base
commits contain every imported change. Original notices and dependency licenses
remain with their sources. Janet modules retain Visualize's MIT license.

On the original development machine, compatibility symlinks keep DrawDo's
`datafriend/` and `littlebear/`, Dinopolis' `datafriend/` and browser assets, and
Visualize's four Janet modules and vendored runtime directory pointing here. They are local conveniences, not
requirements for cloning or building this repository. Shared containers and vendor
sources were copied, leaving other applications' dependencies in place.

The original nested Datafriend/Littlebear Git metadata and source snapshots are
preserved locally under ignored `.migration/`; they are not part of this repository's
history. Existing build caches were retained, but a new build directory is recommended
because CMake caches absolute source paths.
