#ifndef CT_ARENA_H
#define CT_ARENA_H
#include "base.h"
typedef struct ct_free_slot {
  int next_index;
} ct_free_slot;
typedef struct ct_arena {
  uint8_t *start;
  int high_watermark_index, size;
  size_t slab_size;
  ct_free_slot free_slab_head;
} ct_arena;
static inline bool ct_arena_bad(const ct_arena *a) { return !a->start; }
static inline ct_arena ct_arena_create(size_t slab_size, int bytes) {
  ct_arena a = CT_ZERO;
  a.free_slab_head.next_index = -1;
  if (slab_size < sizeof(ct_free_slot) || slab_size % sizeof(int) || bytes <= 0 ||
      slab_size > (size_t)bytes)
    return a;
  a.start = (uint8_t *)malloc((size_t)bytes);
  a.size = bytes / (int)slab_size;
  a.slab_size = slab_size;
  return a;
}
static inline void ct_arena_destroy(ct_arena *a) {
  free(a->start);
  memset(a, 0, sizeof(*a));
  a->free_slab_head.next_index = -1;
}
static inline bool ct_arena_grow(ct_arena *a) {
  int size;
  void *memory;
  if (!a->start || a->size <= 0 || a->size > INT_MAX / 2 ||
      a->slab_size > SIZE_MAX / ((size_t)a->size * 2))
    return false;
  size = a->size * 2;
  memory = realloc(a->start, (size_t)size * a->slab_size);
  if (!memory)
    return false;
  a->start = (uint8_t *)memory;
  a->size = size;
  return true;
}
static inline void *ct_arena_at(ct_arena *a, int index) {
  return a->start + (size_t)index * a->slab_size;
}
static inline int ct_arena_reserve(ct_arena *a) {
  int index;
  if (!a->start)
    return -1;
  index = a->free_slab_head.next_index;
  if (index != -1) {
    a->free_slab_head.next_index = ((ct_free_slot *)ct_arena_at(a, index))->next_index;
    return index;
  }
  if (a->high_watermark_index < a->size || ct_arena_grow(a))
    return a->high_watermark_index++;
  return -1;
}
static inline void ct_arena_release(ct_arena *a, int index) {
  ct_free_slot *s = (ct_free_slot *)ct_arena_at(a, index);
  s->next_index = a->free_slab_head.next_index;
  a->free_slab_head.next_index = index;
}
#endif
