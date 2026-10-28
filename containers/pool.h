#ifndef CT_NODES_H
#define CT_NODES_H
#include "base.h"
enum { CT_SLAB_BYTES = 65536, CT_MAX_BLOCK = 4096 };
typedef struct ct_slab {
  struct ct_slab *prev, *next, *all_prev, *all_next;
  void *free;
  size_t block_bytes, issued, live, capacity;
} ct_slab;
typedef struct ct_bucket {
  ct_slab *available, *all, *spare;
} ct_bucket;
typedef struct ct_pool_stats {
  size_t reserved_bytes, peak_reserved_bytes, live_bytes, allocations, slab_allocations, slab_frees,
      fallback_allocations, fallback_live_bytes;
} ct_pool_stats;
typedef struct ct_pool {
  ct_bucket buckets[64];
  ct_pool_stats stats;
  ct_allocate_fn allocate;
} ct_pool;
static inline void *ct_system_allocate(size_t alignment, size_t bytes) {
  void *p = NULL;
  if (!alignment)
    return malloc(bytes);
  return posix_memalign(&p, alignment, bytes) == 0 ? p : NULL;
}
static inline size_t ct_class_id(size_t bytes) {
  /* Direct lookup for the 24-byte header and 25% slot capacity sequence. */
  static const uint8_t table[513] = {
      0,  0,  0,  0,  0, 0, 0, 1, 2, 3,  4, 5, 0,  6, 0, 7, 0,  0,  8,  0,  0, 9, 0,  0, 0, 10, 0,
      0,  0,  0,  11, 0, 0, 0, 0, 0, 12, 0, 0, 0,  0, 0, 0, 0,  13, 0,  0,  0, 0, 0,  0, 0, 0,  0,
      14, 0,  0,  0,  0, 0, 0, 0, 0, 0,  0, 0, 15, 0, 0, 0, 0,  0,  0,  0,  0, 0, 0,  0, 0, 0,  0,
      16, 0,  0,  0,  0, 0, 0, 0, 0, 0,  0, 0, 0,  0, 0, 0, 0,  0,  0,  17, 0, 0, 0,  0, 0, 0,  0,
      0,  0,  0,  0,  0, 0, 0, 0, 0, 0,  0, 0, 0,  0, 0, 0, 18, 0,  0,  0,  0, 0, 0,  0, 0, 0,  0,
      0,  0,  0,  0,  0, 0, 0, 0, 0, 0,  0, 0, 0,  0, 0, 0, 0,  0,  0,  19, 0, 0, 0,  0, 0, 0,  0,
      0,  0,  0,  0,  0, 0, 0, 0, 0, 0,  0, 0, 0,  0, 0, 0, 0,  0,  0,  0,  0, 0, 0,  0, 0, 0,  0,
      0,  0,  20, 0,  0, 0, 0, 0, 0, 0,  0, 0, 0,  0, 0, 0, 0,  0,  0,  0,  0, 0, 0,  0, 0, 0,  0,
      0,  0,  0,  0,  0, 0, 0, 0, 0, 0,  0, 0, 0,  0, 0, 0, 0,  0,  0,  0,  0, 0, 21, 0, 0, 0,  0,
      0,  0,  0,  0,  0, 0, 0, 0, 0, 0,  0, 0, 0,  0, 0, 0, 0,  0,  0,  0,  0, 0, 0,  0, 0, 0,  0,
      0,  0,  0,  0,  0, 0, 0, 0, 0, 0,  0, 0, 0,  0, 0, 0, 0,  0,  0,  0,  0, 0, 0,  0, 0, 0,  22,
      0,  0,  0,  0,  0, 0, 0, 0, 0, 0,  0, 0, 0,  0, 0, 0, 0,  0,  0,  0,  0, 0, 0,  0, 0, 0,  0,
      0,  0,  0,  0,  0, 0, 0, 0, 0, 0,  0, 0, 0,  0, 0, 0, 0,  0,  0,  0,  0, 0, 0,  0, 0, 0,  0,
      0,  0,  0,  0,  0, 0, 0, 0, 0, 0,  0, 0, 0,  0, 0, 0, 0,  0,  23, 0,  0, 0, 0,  0, 0, 0,  0,
      0,  0,  0,  0,  0, 0, 0, 0, 0, 0,  0, 0, 0,  0, 0, 0, 0,  0,  0,  0,  0, 0, 0,  0, 0, 0,  0,
      0,  0,  0,  0,  0, 0, 0, 0, 0, 0,  0, 0, 0,  0, 0, 0, 0,  0,  0,  0,  0, 0, 0,  0, 0, 0,  0,
      0,  0,  0,  0,  0, 0, 0, 0, 0, 0,  0, 0, 0,  0, 0, 0, 0,  0,  0,  0,  0, 0, 0,  0, 0, 0,  0,
      0,  24, 0,  0,  0, 0, 0, 0, 0, 0,  0, 0, 0,  0, 0, 0, 0,  0,  0,  0,  0, 0, 0,  0, 0, 0,  0,
      0,  0,  0,  0,  0, 0, 0, 0, 0, 0,  0, 0, 0,  0, 0, 0, 0,  0,  0,  0,  0, 0, 0,  0, 0, 0,  0};
  return bytes <= CT_MAX_BLOCK && bytes % 8 == 0 ? table[bytes / 8] : 0;
}
static inline size_t ct_slab_offset(void) { return (sizeof(ct_slab) + 7) & ~(size_t)7; }
static inline void ct_pool_init(ct_pool *p, ct_allocate_fn fn) {
  memset(p, 0, sizeof(*p));
  p->allocate = fn ? fn : ct_system_allocate;
}
static inline void ct_available_add(ct_bucket *b, ct_slab *s) {
  s->prev = NULL;
  s->next = b->available;
  if (s->next)
    s->next->prev = s;
  b->available = s;
}
static inline void ct_available_remove(ct_bucket *b, ct_slab *s) {
  if (s->prev)
    s->prev->next = s->next;
  else
    b->available = s->next;
  if (s->next)
    s->next->prev = s->prev;
  s->prev = s->next = NULL;
}
static inline void ct_pool_peak(ct_pool *p) {
  if (p->stats.reserved_bytes > p->stats.peak_reserved_bytes)
    p->stats.peak_reserved_bytes = p->stats.reserved_bytes;
}
static inline ct_slab *ct_slab_make(ct_pool *p, ct_bucket *b, size_t bytes) {
  ct_slab *s = (ct_slab *)p->allocate(CT_SLAB_BYTES, CT_SLAB_BYTES);
  if (!s)
    return NULL;
  memset(s, 0, sizeof(*s));
  s->block_bytes = bytes;
  s->capacity = (CT_SLAB_BYTES - ct_slab_offset()) / bytes;
  s->all_next = b->all;
  if (b->all)
    b->all->all_prev = s;
  b->all = s;
  ct_available_add(b, s);
  p->stats.reserved_bytes += CT_SLAB_BYTES;
  ++p->stats.slab_allocations;
  ct_pool_peak(p);
  return s;
}
static inline void ct_slab_drop(ct_pool *p, ct_bucket *b, ct_slab *s) {
  assert(!s->live);
  ct_available_remove(b, s);
  if (s->all_prev)
    s->all_prev->all_next = s->all_next;
  else
    b->all = s->all_next;
  if (s->all_next)
    s->all_next->all_prev = s->all_prev;
  if (b->spare == s)
    b->spare = NULL;
  free(s);
  p->stats.reserved_bytes -= CT_SLAB_BYTES;
  ++p->stats.slab_frees;
}
static inline void *ct_pool_allocate(ct_pool *p, size_t bytes) {
  size_t id = ct_class_id(bytes);
  ct_bucket *b;
  ct_slab *s;
  void *out;
  if (!p->allocate)
    p->allocate = ct_system_allocate;
  if (!id) {
    out = p->allocate(0, bytes);
    if (!out)
      return NULL;
    ++p->stats.allocations;
    p->stats.live_bytes += bytes;
    ++p->stats.fallback_allocations;
    p->stats.fallback_live_bytes += bytes;
    p->stats.reserved_bytes += bytes;
    ct_pool_peak(p);
    return out;
  }
  b = &p->buckets[id];
  s = b->available ? b->available : ct_slab_make(p, b, bytes);
  if (!s)
    return NULL;
  ++p->stats.allocations;
  p->stats.live_bytes += bytes;
  assert(s->block_bytes == bytes);
  if (b->spare == s)
    b->spare = NULL;
  if (s->free) {
    out = s->free;
    memcpy(&s->free, out, sizeof(void *));
  } else
    out = (char *)s + ct_slab_offset() + (s->issued++) * bytes;
  if (++s->live == s->capacity)
    ct_available_remove(b, s);
  return out;
}
static inline void ct_pool_deallocate(ct_pool *p, void *memory, size_t bytes) {
  size_t id = ct_class_id(bytes);
  ct_slab *s;
  ct_bucket *b;
  assert(memory && p->stats.live_bytes >= bytes);
  p->stats.live_bytes -= bytes;
  if (!id) {
    free(memory);
    p->stats.reserved_bytes -= bytes;
    p->stats.fallback_live_bytes -= bytes;
    return;
  }
  s = (ct_slab *)((uintptr_t)memory & ~(uintptr_t)(CT_SLAB_BYTES - 1));
  b = &p->buckets[id];
  assert(s->block_bytes == bytes && s->live);
  if (s->live == s->capacity)
    ct_available_add(b, s);
  memcpy(memory, &s->free, sizeof(void *));
  s->free = memory;
  if (--s->live == 0) {
    if (b->spare)
      ct_slab_drop(p, b, s);
    else
      b->spare = s;
  }
}
static inline void ct_pool_trim(ct_pool *p) {
  size_t i;
  for (i = 0; i < 64; ++i)
    if (p->buckets[i].spare)
      ct_slab_drop(p, &p->buckets[i], p->buckets[i].spare);
}
static inline void ct_pool_destroy(ct_pool *p) {
  assert(!p->stats.live_bytes);
  ct_pool_trim(p);
}
#endif
