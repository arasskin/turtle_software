#ifndef LB_INDEX_H
#define LB_INDEX_H
#include "../containers/tree.h"
#include "../containers/pool.h"
#ifndef XXH_INLINE_ALL
#define XXH_INLINE_ALL
#endif
#include "../third_party/xxhash/xxhash.h"
#define LB_OFFSET_MASK UINT64_C(0x0000FFFFFFFFFFFF)
typedef struct lb_index {
  ct_node *root;
  ct_pool *node_allocator;
  unsigned rank;
} lb_index;
static inline uint64_t lb_vpointer(uint64_t offset, uint16_t size) {
  return (offset & LB_OFFSET_MASK) | ((uint64_t)size << 48);
}
static inline uint64_t lb_voffset(uint64_t p) {
  return p & LB_OFFSET_MASK;
}
static inline uint16_t lb_vsize(uint64_t p) {
  return (uint16_t)(p >> 48);
}
static inline uint64_t lb_key_hash(uint64_t k) {
  uint8_t b[8];
  unsigned i;
  for (i = 0; i < 8; ++i)
    b[i] = (uint8_t)(k >> (56 - 8 * i));
  return XXH64(b, sizeof(b), 0);
}
static inline void *lb_index_allocate(void *p, size_t n) {
  return ct_pool_allocate((ct_pool *)p, n);
}
static inline void lb_index_release(void *p, void *memory, size_t n) {
  ct_pool_deallocate((ct_pool *)p, memory, n);
}
static inline ct_tree lb_index_view(const lb_index *h) {
  ct_tree t = CT_ZERO;
  t.root = h->root;
  t.rank = h->rank;
  t.key_hash = lb_key_hash;
  t.allocator.context = h->node_allocator;
  t.allocator.allocate = lb_index_allocate;
  t.allocator.deallocate = lb_index_release;
  return t;
}
static inline lb_index lb_index_create(ct_pool *pool) {
  lb_index h = CT_ZERO;
  h.node_allocator = pool;
  return h;
}
static inline void lb_index_destroy(lb_index *h) {
  ct_tree t = lb_index_view(h);
  ct_tree_destroy(&t);
  memset(h, 0, sizeof(*h));
}
static inline bool lb_index_find(const lb_index *h, uint64_t k, ct_record *out) {
  ct_tree t = lb_index_view(h);
  return ct_tree_get(&t, k, out);
}
static inline uint64_t lb_index_get(const lb_index *h, uint64_t k) {
  ct_record r;
  return lb_index_find(h, k, &r) ? r.value : 0;
}
static inline bool lb_index_replace(lb_index *h, uint64_t k, uint64_t value, uint64_t hash) {
  ct_record r = {k, value, hash};
  return ct_node_replace(h->root, h->rank, r);
}
static inline int lb_index_set(lb_index *h, uint64_t k, uint64_t value, uint64_t hash) {
  ct_tree t = lb_index_view(h);
  ct_record r = {k, value, hash};
  int rc = ct_tree_set(&t, r);
  h->root = t.root;
  h->rank = t.rank;
  return rc;
}
static inline int lb_index_erase(lb_index *h, uint64_t k) {
  ct_tree t = lb_index_view(h);
  int rc = ct_tree_erase(&t, k);
  h->root = t.root;
  h->rank = t.rank;
  return rc;
}
static inline ct_summary lb_index_summary(const lb_index *h, ct_range r) {
  ct_tree t = lb_index_view(h);
  return ct_tree_summary(&t, r);
}
static inline bool lb_index_select(const lb_index *h, uint64_t n, ct_record *out) {
  ct_tree t = lb_index_view(h);
  return ct_tree_select(&t, n, out);
}
typedef char lb_index_layout_check[sizeof(lb_index)==24?1:-1];
#endif
