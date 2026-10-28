# Wide-tree source provenance

LittleBear's mutable wide index uses the shared implementation at
`containers/tree.h`, adapted from ClojureDart's
`HashRankedWideTreapMap`.

Original copyright (c) Baptiste Dupuch & Christophe Grand. All rights reserved.
See [the shared source notice](../datafriend/NOTICE.md) and the included
[Eclipse Public License 1.0](../datafriend/licenses/epl-v10.html).

The backend facade, slab pool and allocation-failure integration preserve that
provenance. The repository's bundled xxHash retains its own BSD-2-Clause notice.

LittleBear's namespace directory is a C99 dense array with a linear-probing index.
The server no longer uses `ankerl::unordered_dense`.
