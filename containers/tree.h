/* Mutable adaptation of ClojureDart's HashRankedWideTreapMap algorithm.
 * Copyright (c) Baptiste Dupuch & Christophe Grand. All rights reserved.
 * Eclipse Public License 1.0; see datafriend/licenses/epl-v10.html and NOTICE.md.
 * C adaptation and count/fingerprint augmentation: DrawDo. */
#ifndef CT_TREE_H
#define CT_TREE_H
#include "base.h"
typedef struct ct_record {
  uint64_t key, value, hash;
} ct_record;
typedef struct ct_range {
  uint64_t low, high;
  bool bounded;
} ct_range;
typedef struct ct_summary {
  uint64_t count, hash;
} ct_summary;
typedef struct ct_node ct_node;
typedef union ct_slot {
  uint64_t word;
  ct_node *child;
} ct_slot;
struct ct_node {
  uint64_t count, hash;
  uint32_t size, capacity;
};
typedef struct ct_tree_stats {
  size_t live_bytes, peak_bytes, live_nodes, allocations, copied_bytes;
} ct_tree_stats;
typedef struct ct_node_allocator {
  void *context;
  void *(*allocate)(void *, size_t);
  void (*deallocate)(void *, void *, size_t);
} ct_node_allocator;
/* A view owns no metadata allocation; namespace headers carry root and rank.
 * All fallible mutations return -1 and preserve logical contents on failure. */
typedef struct ct_tree {
  ct_node *root;
  unsigned rank;
  uint64_t (*key_hash)(uint64_t);
  ct_node_allocator allocator;
  ct_tree_stats *stats;
} ct_tree;
static inline ct_slot *ct_data(ct_node *n) { return (ct_slot *)(n + 1); }
static inline const ct_slot *ct_cdata(const ct_node *n) { return (const ct_slot *)(n + 1); }
static inline uint64_t ct_node_count(const ct_node *n) { return n ? n->count : 0; }
static inline uint64_t ct_node_hash(const ct_node *n) { return n ? n->hash : 0; }
static inline unsigned ct_tree_rank(const ct_tree *t, uint64_t k) {
  return ct_ctz32((uint32_t)t->key_hash(k)) / 4;
}
static inline ct_record ct_entry(const ct_node *n, size_t i) {
  ct_record r = {ct_cdata(n)[i].word, ct_cdata(n)[i + 1].word, ct_cdata(n)[i + 2].word};
  return r;
}
static inline void ct_copied(ct_tree *t, size_t n) {
  if (t->stats)
    t->stats->copied_bytes += n * sizeof(ct_slot);
}
static inline ct_node *ct_node_make(ct_tree *t, size_t size) {
  size_t capacity = 4, bytes;
  ct_node *n;
  while (capacity < size) {
    if (capacity > UINT32_MAX)
      return NULL;
    capacity += CT_MAX((size_t)1, capacity / 4);
  }
  if (capacity > UINT32_MAX || capacity > (SIZE_MAX - sizeof(ct_node)) / sizeof(ct_slot))
    return NULL;
  bytes = sizeof(ct_node) + capacity * sizeof(ct_slot);
  n = (ct_node *)t->allocator.allocate(t->allocator.context, bytes);
  if (!n)
    return NULL;
  n->count = n->hash = 0;
  n->size = (uint32_t)size;
  n->capacity = (uint32_t)capacity;
  if (t->stats) {
    t->stats->live_bytes += bytes;
    t->stats->peak_bytes = CT_MAX(t->stats->peak_bytes, t->stats->live_bytes);
    ++t->stats->live_nodes;
    ++t->stats->allocations;
  }
  return n;
}
static inline void ct_node_release(ct_tree *t, ct_node *n) {
  size_t bytes;
  if (!n)
    return;
  bytes = sizeof(ct_node) + n->capacity * sizeof(ct_slot);
  if (t->stats) {
    t->stats->live_bytes -= bytes;
    --t->stats->live_nodes;
  }
  t->allocator.deallocate(t->allocator.context, n, bytes);
}
static inline void ct_node_destroy(ct_tree *t, ct_node *n, unsigned rank) {
  size_t i;
  if (!n)
    return;
  if (rank)
    for (i = 0; i < n->size; i += 2)
      ct_node_destroy(t, ct_data(n)[i].child, rank - 1);
  ct_node_release(t, n);
}
static inline void ct_tree_destroy(ct_tree *t) {
  ct_node_destroy(t, t->root, t->rank);
  t->root = NULL;
  t->rank = 0;
}
static inline bool ct_node_reserve(ct_tree *t, ct_node **p, size_t size) {
  ct_node *n = *p, *next;
  if (n && n->capacity >= size)
    return true;
  next = ct_node_make(t, size);
  if (!next)
    return false;
  next->size = n ? n->size : 0;
  if (n) {
    next->count = n->count;
    next->hash = n->hash;
    memcpy(ct_data(next), ct_data(n), n->size * sizeof(ct_slot));
    ct_copied(t, n->size);
    ct_node_release(t, n);
  }
  *p = next;
  return true;
}
static inline void ct_node_trim(ct_tree *t, ct_node **p) {
  ct_node *n = *p, *next;
  if (!n)
    return;
  if (!n->count) {
    ct_node_destroy(t, n, 0);
    *p = NULL;
    return;
  }
  if (n->size >= n->capacity / 2 || n->capacity <= 8)
    return;
  next = ct_node_make(t, n->size);
  if (!next)
    return;
  next->count = n->count;
  next->hash = n->hash;
  memcpy(ct_data(next), ct_data(n), n->size * sizeof(ct_slot));
  ct_copied(t, n->size);
  ct_node_release(t, n);
  *p = next;
}
static inline size_t ct_leaf_lower(const ct_node *n, uint64_t k) {
  size_t lo = 0, hi = n->size / 3;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (ct_cdata(n)[3 * mid].word < k)
      lo = mid + 1;
    else
      hi = mid;
  }
  return 3 * lo;
}
static inline size_t ct_branch(const ct_node *n, uint64_t k) {
  size_t lo = 0, hi = n->size / 2;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (ct_cdata(n)[2 * mid + 1].word <= k)
      lo = mid + 1;
    else
      hi = mid;
  }
  return lo;
}
static inline void ct_refresh(ct_node *n, unsigned rank) {
  size_t i;
  n->count = n->hash = 0;
  if (rank)
    for (i = 0; i < n->size; i += 2) {
      n->count += ct_node_count(ct_data(n)[i].child);
      n->hash ^= ct_node_hash(ct_data(n)[i].child);
    }
  else {
    n->count = n->size / 3;
    for (i = 2; i < n->size; i += 3)
      n->hash ^= ct_data(n)[i].word;
  }
}
static inline bool ct_node_replace(ct_node *n, unsigned rank, ct_record r) {
  size_t i;
  ct_node *child;
  uint64_t before;
  if (!n)
    return false;
  if (!rank) {
    i = ct_leaf_lower(n, r.key);
    if (i == n->size || ct_data(n)[i].word != r.key)
      return false;
    n->hash ^= ct_data(n)[i + 2].word ^ r.hash;
    ct_data(n)[i + 1].word = r.value;
    ct_data(n)[i + 2].word = r.hash;
    return true;
  }
  child = ct_data(n)[2 * ct_branch(n, r.key)].child;
  before = ct_node_hash(child);
  if (!ct_node_replace(child, rank - 1, r))
    return false;
  n->hash ^= before ^ ct_node_hash(child);
  return true;
}
static inline bool ct_leaf_insert(ct_tree *t, ct_node **p, ct_record r, size_t i) {
  size_t old = *p ? (*p)->size : 0;
  ct_node *n;
  if (!ct_node_reserve(t, p, old + 3))
    return false;
  n = *p;
  memmove(ct_data(n) + i + 3, ct_data(n) + i, (old - i) * sizeof(ct_slot));
  ct_copied(t, old - i);
  ct_data(n)[i].word = r.key;
  ct_data(n)[i + 1].word = r.value;
  ct_data(n)[i + 2].word = r.hash;
  n->size += 3;
  ++n->count;
  n->hash ^= r.hash;
  return true;
}
static inline bool ct_split_insert(ct_tree *t, ct_node *old, unsigned rank, ct_record r,
                                   ct_node **left_out, ct_node **right_out) {
  size_t i;
  ct_node *right, *left_child, *right_child;
  if (!rank) {
    i = old ? ct_leaf_lower(old, r.key) : 0;
    if (!i) {
      if (!ct_leaf_insert(t, &old, r, 0))
        return false;
      *left_out = NULL;
      *right_out = old;
      return true;
    }
    right = ct_node_make(t, old->size - i + 3);
    if (!right)
      return false;
    ct_data(right)[0].word = r.key;
    ct_data(right)[1].word = r.value;
    ct_data(right)[2].word = r.hash;
    memcpy(ct_data(right) + 3, ct_data(old) + i, (old->size - i) * sizeof(ct_slot));
    ct_copied(t, old->size - i);
    old->size = (uint32_t)i;
    ct_refresh(old, 0);
    ct_refresh(right, 0);
    ct_node_trim(t, &old);
    *left_out = old;
    *right_out = right;
    return true;
  }
  i = old ? 2 * ct_branch(old, r.key) : 0;
  right = ct_node_make(t, old ? old->size - i : 1);
  if (!right)
    return false;
  if (!ct_split_insert(t, old ? ct_data(old)[i].child : NULL, rank - 1, r, &left_child,
                       &right_child)) {
    ct_node_release(t, right);
    return false;
  }
  if (old) {
    memcpy(ct_data(right), ct_data(old) + i, right->size * sizeof(ct_slot));
    ct_copied(t, right->size);
    ct_data(old)[i].child = left_child;
    old->size = (uint32_t)(i + 1);
    ct_refresh(old, rank);
    if (!old->count) {
      ct_node_destroy(t, old, rank);
      old = NULL;
    } else
      ct_node_trim(t, &old);
  }
  ct_data(right)[0].child = right_child;
  ct_refresh(right, rank);
  *left_out = old;
  *right_out = right;
  return true;
}
static inline bool ct_node_insert(ct_tree *t, ct_node **p, unsigned rank, unsigned kr,
                                  ct_record r) {
  bool created;
  size_t i, old;
  ct_node *n, *left, *right;
  if (!rank)
    return ct_leaf_insert(t, p, r, *p ? ct_leaf_lower(*p, r.key) : 0);
  created = !*p;
  if (created) {
    *p = ct_node_make(t, 1);
    if (!*p)
      return false;
    ct_data(*p)[0].child = NULL;
  }
  n = *p;
  i = 2 * ct_branch(n, r.key);
  if (rank == kr) {
    old = n->size;
    if (!ct_node_reserve(t, p, old + 2))
      goto fail;
    n = *p;
    if (!ct_split_insert(t, ct_data(n)[i].child, rank - 1, r, &left, &right))
      goto fail;
    memmove(ct_data(n) + i + 3, ct_data(n) + i + 1, (old - i - 1) * sizeof(ct_slot));
    ct_copied(t, old - i - 1);
    ct_data(n)[i].child = left;
    ct_data(n)[i + 1].word = r.key;
    ct_data(n)[i + 2].child = right;
    n->size += 2;
  } else if (!ct_node_insert(t, &ct_data(n)[i].child, rank - 1, kr, r))
    goto fail;
  ++n->count;
  n->hash ^= r.hash;
  return true;
fail:
  if (created) {
    ct_node_release(t, *p);
    *p = NULL;
  }
  return false;
}
static inline int ct_tree_set(ct_tree *t, ct_record r) {
  unsigned rank, old_top;
  if (ct_node_replace(t->root, t->rank, r))
    return 0;
  rank = ct_tree_rank(t, r.key);
  old_top = t->rank;
  while (t->rank < rank) {
    ct_node *parent = ct_node_make(t, 1);
    if (!parent)
      goto fail;
    ct_data(parent)[0].child = t->root;
    ct_refresh(parent, ++t->rank);
    t->root = parent;
  }
  if (ct_node_insert(t, &t->root, t->rank, rank, r))
    return 1;
fail:
  while (t->rank > old_top) {
    ct_node *child = ct_data(t->root)[0].child;
    ct_node_release(t, t->root);
    t->root = child;
    --t->rank;
  }
  return -1;
}
static inline bool ct_prepare_join(ct_tree *t, ct_node **left, const ct_node *right,
                                   unsigned rank) {
  if (!*left || !right || right->count == 1)
    return true;
  if (!rank)
    return ct_node_reserve(t, left, (size_t)(*left)->size + right->size - 3);
  if (!ct_node_reserve(t, left, (size_t)(*left)->size + right->size - 1))
    return false;
  return ct_prepare_join(t, &ct_data(*left)[(*left)->size - 1].child, ct_cdata(right)[0].child,
                         rank - 1);
}
static inline ct_node *ct_join(ct_tree *t, ct_node *left, ct_node *right, unsigned rank) {
  size_t old;
  if (!left)
    return right;
  if (!right)
    return left;
  old = left->size;
  if (!rank) {
    assert(left->capacity >= old + right->size);
    memcpy(ct_data(left) + old, ct_data(right), right->size * sizeof(ct_slot));
    ct_copied(t, right->size);
    left->size += right->size;
  } else {
    ct_node *middle = ct_join(t, ct_data(left)[old - 1].child, ct_data(right)[0].child, rank - 1);
    assert(left->capacity >= old + right->size - 1);
    ct_data(left)[old - 1].child = middle;
    memcpy(ct_data(left) + old, ct_data(right) + 1, (right->size - 1) * sizeof(ct_slot));
    ct_copied(t, right->size - 1);
    left->size += right->size - 1;
  }
  ct_node_release(t, right);
  ct_refresh(left, rank);
  return left;
}
static inline int ct_node_remove(ct_tree *t, ct_node **p, unsigned rank, uint64_t k) {
  ct_node *n = *p;
  size_t i;
  int rc;
  if (!n)
    return 0;
  if (!rank) {
    i = ct_leaf_lower(n, k);
    if (i == n->size || ct_data(n)[i].word != k)
      return 0;
    n->hash ^= ct_data(n)[i + 2].word;
    --n->count;
    memmove(ct_data(n) + i, ct_data(n) + i + 3, (n->size - i - 3) * sizeof(ct_slot));
    ct_copied(t, n->size - i - 3);
    n->size -= 3;
    ct_node_trim(t, p);
    return 1;
  }
  i = 2 * ct_branch(n, k);
  if (i && ct_data(n)[i - 1].word == k &&
      !ct_prepare_join(t, &ct_data(n)[i - 2].child, ct_data(n)[i].child, rank - 1))
    return -1;
  rc = ct_node_remove(t, &ct_data(n)[i].child, rank - 1, k);
  if (rc != 1)
    return rc;
  if (i && ct_data(n)[i - 1].word == k) {
    ct_data(n)[i - 2].child = ct_join(t, ct_data(n)[i - 2].child, ct_data(n)[i].child, rank - 1);
    memmove(ct_data(n) + i - 1, ct_data(n) + i + 1, (n->size - i - 1) * sizeof(ct_slot));
    ct_copied(t, n->size - i - 1);
    n->size -= 2;
  }
  ct_refresh(n, rank);
  if (!n->count) {
    ct_node_destroy(t, n, rank);
    *p = NULL;
  } else
    ct_node_trim(t, p);
  return 1;
}
static inline int ct_tree_erase(ct_tree *t, uint64_t k) {
  int rc = ct_node_remove(t, &t->root, t->rank, k);
  if (rc != 1)
    return rc;
  while (t->root && t->rank && t->root->size == 1) {
    ct_node *child = ct_data(t->root)[0].child;
    ct_node_release(t, t->root);
    t->root = child;
    --t->rank;
  }
  if (!t->root)
    t->rank = 0;
  return 1;
}
static inline bool ct_tree_get(const ct_tree *t, uint64_t k, ct_record *out) {
  const ct_node *n = t->root;
  unsigned rank;
  size_t i;
  for (rank = t->rank; n && rank; --rank)
    n = ct_cdata(n)[2 * ct_branch(n, k)].child;
  if (!n)
    return false;
  i = ct_leaf_lower(n, k);
  if (i == n->size || ct_cdata(n)[i].word != k)
    return false;
  *out = ct_entry(n, i);
  return true;
}
static inline bool ct_tree_select(const ct_tree *t, uint64_t index, ct_record *out) {
  const ct_node *n = t->root;
  unsigned rank;
  if (index >= ct_node_count(n))
    return false;
  for (rank = t->rank; rank; --rank) {
    size_t i = 0;
    while (index >= ct_node_count(ct_cdata(n)[i].child)) {
      index -= ct_node_count(ct_cdata(n)[i].child);
      i += 2;
      assert(i < n->size);
    }
    n = ct_cdata(n)[i].child;
  }
  *out = ct_entry(n, (size_t)index * 3);
  return true;
}
static inline ct_summary ct_tree_prefix(const ct_tree *t, uint64_t k) {
  ct_summary out = CT_ZERO;
  const ct_node *n = t->root;
  unsigned rank;
  size_t i, end;
  for (rank = t->rank; n && rank; --rank) {
    end = 2 * ct_branch(n, k);
    for (i = 0; i < end; i += 2) {
      out.count += ct_node_count(ct_cdata(n)[i].child);
      out.hash ^= ct_node_hash(ct_cdata(n)[i].child);
    }
    n = ct_cdata(n)[end].child;
  }
  if (n) {
    end = ct_leaf_lower(n, k);
    out.count += end / 3;
    for (i = 2; i < end; i += 3)
      out.hash ^= ct_cdata(n)[i].word;
  }
  return out;
}
static inline ct_summary ct_tree_summary(const ct_tree *t, ct_range r) {
  ct_summary lo, hi, out = CT_ZERO;
  if (r.bounded && r.low >= r.high)
    return out;
  lo = ct_tree_prefix(t, r.low);
  if (r.bounded)
    hi = ct_tree_prefix(t, r.high);
  else {
    hi.count = ct_node_count(t->root);
    hi.hash = ct_node_hash(t->root);
  }
  out.count = hi.count - lo.count;
  out.hash = hi.hash ^ lo.hash;
  return out;
}
static inline void ct_collect(const ct_node *n, unsigned rank, ct_range r, ct_record *out,
                              size_t capacity, size_t *used) {
  size_t i;
  if (!n)
    return;
  if (!rank) {
    for (i = ct_leaf_lower(n, r.low); i < n->size && *used < capacity; i += 3) {
      ct_record e = ct_entry(n, i);
      if (r.bounded && e.key >= r.high)
        break;
      out[(*used)++] = e;
    }
    return;
  }
  for (i = 2 * ct_branch(n, r.low); i < n->size && *used < capacity; i += 2) {
    if (i && r.bounded && ct_cdata(n)[i - 1].word >= r.high)
      break;
    ct_collect(ct_cdata(n)[i].child, rank - 1, r, out, capacity, used);
  }
}
static inline size_t ct_tree_list(const ct_tree *t, ct_range r, ct_record *out, size_t capacity) {
  size_t used = 0;
  if (!r.bounded || r.low < r.high)
    ct_collect(t->root, t->rank, r, out, capacity, &used);
  return used;
}
static inline bool ct_inspect(const ct_tree *t, const ct_node *n, unsigned rank, ct_range r,
                              ct_summary *out) {
  size_t i;
  out->count = out->hash = 0;
  if (!n)
    return true;
  if (!n->size || n->size > n->capacity)
    return false;
  if (!rank) {
    if (n->size % 3)
      return false;
    for (i = 0; i < n->size; i += 3) {
      ct_record e = ct_entry(n, i);
      if (e.key < r.low || (r.bounded && e.key >= r.high) ||
          (i && ct_cdata(n)[i - 3].word >= e.key))
        return false;
      ++out->count;
      out->hash ^= e.hash;
    }
  } else {
    if (n->size % 2 != 1)
      return false;
    for (i = 1; i < n->size; i += 2) {
      uint64_t k = ct_cdata(n)[i].word;
      const ct_node *first = ct_cdata(n)[i + 1].child;
      unsigned d;
      if (ct_tree_rank(t, k) != rank || k < r.low || (r.bounded && k >= r.high) ||
          (i > 1 && ct_cdata(n)[i - 2].word >= k))
        return false;
      for (d = rank - 1; first && d; --d)
        first = ct_cdata(first)[0].child;
      if (!first || ct_cdata(first)[0].word != k)
        return false;
    }
    for (i = 0; i < n->size; i += 2) {
      ct_range child = r;
      ct_summary sum;
      if (i)
        child.low = ct_cdata(n)[i - 1].word;
      if (i + 1 < n->size) {
        child.high = ct_cdata(n)[i + 1].word;
        child.bounded = true;
      }
      if (!ct_inspect(t, ct_cdata(n)[i].child, rank - 1, child, &sum))
        return false;
      out->count += sum.count;
      out->hash ^= sum.hash;
    }
  }
  return out->count == n->count && out->hash == n->hash;
}
static inline bool ct_tree_validate(const ct_tree *t) {
  ct_summary s;
  ct_range r = CT_ZERO;
  return ct_inspect(t, t->root, t->rank, r, &s);
}
static inline uint64_t ct_tree_shape(const ct_node *n, unsigned rank) {
  uint64_t h;
  size_t i;
  if (!n)
    return 0xCAFE;
  h = n->size + (uint64_t)rank * 131;
  for (i = 0; i < n->size; i += rank ? 1 : 3) {
    uint64_t x =
        rank && i % 2 == 0 ? ct_tree_shape(ct_cdata(n)[i].child, rank - 1) : ct_cdata(n)[i].word;
    h = ((h << 7) | (h >> 57)) ^ x;
  }
  return h;
}
typedef char ct_tree_layout_check
    [(sizeof(ct_node) == 24 && sizeof(ct_slot) == 8 && sizeof(ct_record) == 24) ? 1 : -1];
#endif
