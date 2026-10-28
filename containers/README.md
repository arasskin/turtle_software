# Shared C containers

Header-only C99 primitives used by Littlebear, Datafriend and the native app.
Include the needed header directly; no library, generator or registration step.
They depend only on C/POSIX facilities and each other, never on the applications.
The repositories use these sibling sources just as they use `third_party/`.
Consumers of `pool.h` must enable POSIX declarations before system headers
(for example, `_POSIX_C_SOURCE=200809L`) for `posix_memalign`.

| Header | Purpose |
| --- | --- |
| `array.h` | Geometric growth for a typed pointer and element capacity, with checked multiplication and an optional element limit |
| `bytes.h` | NUL-terminated byte storage with reusable capacity and explicit per-call failure; used by text and native UI libraries |
| `buffer.h` | Binary byte buffer with sticky errors, capacity reuse and optional byte limit |
| `map.h` | Dense `uint64_t → uintptr_t` map, 32-bit probing indices, backward-shift deletion; all 64-bit keys are valid |
| `tree.h` | Hash-ranked wide tree with record keys, values, fingerprints, rank/select and range summaries |
| `pool.h` | Stable-address size-class slab allocator used by tree nodes; POSIX aligned allocation |
| `arena.h` | Growable fixed-size slots with an index free list; used for value-file allocator metadata |
| `base.h` | Small shared helpers and allocator callback type for the tree/pools |

Public names use `ct_` / `CT_`. Storage is explicit, functions are `static inline`,
and allocation failures return to the caller. No per-element destructors run:
the owner releases payloads before releasing a container. Maps store non-owning
integer/pointer values. Tree allocation is supplied through callbacks.

## Array contract

```c
#include "containers/array.h"
struct item { unsigned id; };
struct item *items = NULL;
size_t count = 0, capacity = 0;
if (ct_array_reserve(&items, &capacity, count + 1, sizeof(*items)))
  items[count++] = (struct item){42};
free(items);
```

Capacity counts elements. Newly allocated slots are uninitialized. Failed growth
preserves existing contents, pointer and capacity. Successful growth may move
storage: retain indices and reacquire pointers after reserve or callbacks that
can reserve. The implementation copies the pointer representation rather than
accessing a typed pointer through `void **`. The supported native platforms use
the same representation for object pointers.

For raw arrays, callers maintain the element count and check additions before
they can wrap. Map operations maintain their own count; growth invalidates entry
pointers and deletion can move the final entry. Buffers perform their own
addition checks; append input must not alias the
buffer because growth can move it. `ct_buffer_clear` resets size/error and retains
capacity; `ct_buffer_free` releases storage and resets all fields.

## Boundaries

The byte buffer has no wire-format knowledge. Littlebear's codec retains its
frame bound and both codecs retain their own encoding. The app and native UI libraries share the NUL-terminated `bytes.h` storage;
app-specific formatting and wire encoding remain with the app. Littlebear's
namespace directory remains specialized around namespace/index ownership, and
its file allocator remains in `heap.h`. The renderer's texture cache remains with
rendering because it owns GPU resources. Historical benchmark worktrees keep
their own implementations.

## Validation and provenance

`make -C containers test` checks overflow and allocation-failure behavior;
`make -C containers sanitize` runs it with ASan/UBSan. The existing Littlebear
model/failure tests cover the tree and pools, and Datafriend's C99 tests cover
randomized dense-map churn. The shared tests include every header as strict C99.

The wide tree retains its ClojureDart attribution and EPL-1.0 terms. See
[`../datafriend/NOTICE.md`](../datafriend/NOTICE.md) and the included
[EPL-1.0 license](../datafriend/licenses/epl-v10.html). Extraction does not change
its algorithm, record layout or licensing.
