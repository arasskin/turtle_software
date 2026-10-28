# Source provenance

The mutable wide tree adapts the hash-ranked wide-tree algorithm and leaf-only
value layout from ClojureDart's `HashRankedWideTreapMap`, in
`clj/src/cljd/core.cljd` at commit
`4ee792f3044589c96b019c2a86db9b23f1b283ab`.

Original copyright (c) Baptiste Dupuch & Christophe Grand. All rights reserved.
The original source is covered by the Eclipse Public License 1.0; a copy is
included in [licenses/epl-v10.html](licenses/epl-v10.html). Its notices must not
be removed. The adaptation adds mutable allocation, per-entry fingerprints,
subtree XOR summaries, rank/select, and bounded range enumeration.

Datafriend's C99 engine uses the bundled xxHash header (BSD-2-Clause) and
the pinned `third_party/aelmdb` sources and license. AELMDB's XOR aggregate and distinct format-tag changes are documented
with upstream hashes in that directory. Local view reconciliation uses the
independent shared `littlebear/reconcile.h` implementation (LBR3).

Native owner identities use Monocypher 4.0.3 core and optional Ed25519 sources,
vendor-copied unchanged under `third_party/monocypher`, with their dual BSD-2-Clause /
CC0 license and upstream SHA-512 archive checksum in `UPSTREAM.json`. The SHA-512
Ed25519 API is used for RFC 8032 compatibility with Littlebear's libsodium verifier.

The mutable wide-tree and slab allocation implementation now lives in
`../containers/tree.h` and `../containers/pool.h` as C99.
The original wide-tree attribution and EPL-1.0 terms remain in the shared source.
