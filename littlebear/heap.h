#ifndef LB_HEAP_H
#define LB_HEAP_H
#include "../containers/arena.h"
#include <fcntl.h>
#include <unistd.h>
enum { LB_SIZE_CLASSES = 89, LB_INVALID_CLASS = UINT8_MAX, LB_MAX_VALUE = UINT16_MAX };
typedef struct lb_heap_slot {
  int next_slab_index;
  uint64_t file_offset;
} lb_heap_slot;
typedef struct lb_heap {
  uint64_t high_watermark, limit;
  lb_heap_slot size_class_heads[LB_SIZE_CLASSES];
  ct_arena slab_allocator;
} lb_heap;
typedef struct lb_file {
  int file_descriptor;
  lb_heap file_allocator;
  bool has_error;
} lb_file;
typedef struct lb_offset {
  uint64_t offset;
  bool has_error;
} lb_offset;
static inline lb_file lb_file_create(const char *path, size_t limit) {
  lb_file f = CT_ZERO;
  size_t i;
  f.file_descriptor = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  f.has_error = true;
  if (f.file_descriptor < 0)
    return f;
  f.file_allocator.slab_allocator = ct_arena_create(sizeof(lb_heap_slot), 8192);
  if (ct_arena_bad(&f.file_allocator.slab_allocator)) {
    close(f.file_descriptor);
    f.file_descriptor = -1;
    return f;
  }
  f.file_allocator.limit = limit;
  for (i = 0; i < LB_SIZE_CLASSES; ++i)
    f.file_allocator.size_class_heads[i].next_slab_index = -1;
  f.has_error = false;
  return f;
}
static inline void lb_file_destroy(lb_file *f) {
  ct_arena_destroy(&f->file_allocator.slab_allocator);
  if (f->file_descriptor >= 0)
    close(f->file_descriptor);
  f->file_descriptor = -1;
}
static inline uint8_t lb_size_class(size_t bytes) {
  uint32_t last;
  unsigned log2, sub;
  if (!bytes || bytes > LB_MAX_VALUE)
    return LB_INVALID_CLASS;
  if (bytes <= 32)
    return 0;
  last = (uint32_t)(bytes - 1);
  log2 = 31 - ct_clz32(last);
  sub = (last >> (log2 - 3)) & 7;
  return (uint8_t)(1 + (log2 - 5) * 8 + sub);
}
static inline uint32_t lb_slot_size(uint8_t index) {
  unsigned log2;
  if (index >= LB_SIZE_CLASSES)
    return 0;
  if (!index)
    return 32;
  --index;
  log2 = index / 8 + 5;
  return (1u << log2) + ((index % 8 + 1u) << (log2 - 3));
}
static inline lb_offset lb_heap_reserve(lb_heap *a, size_t size) {
  lb_offset out = {0, true};
  uint8_t cl = lb_size_class(size);
  lb_heap_slot *head;
  uint32_t fitting;
  int i;
  if (cl == LB_INVALID_CLASS)
    return out;
  head = &a->size_class_heads[cl];
  i = head->next_slab_index;
  if (i != -1) {
    lb_heap_slot *s = (lb_heap_slot *)ct_arena_at(&a->slab_allocator, i);
    out.offset = s->file_offset;
    head->next_slab_index = s->next_slab_index;
    ct_arena_release(&a->slab_allocator, i);
    out.has_error = false;
    return out;
  }
  fitting = lb_slot_size(cl);
  if (a->high_watermark <= a->limit && fitting <= a->limit - a->high_watermark) {
    out.offset = a->high_watermark;
    a->high_watermark += fitting;
    out.has_error = false;
  }
  return out;
}
static inline int lb_heap_prepare(lb_heap *a) {
  return ct_arena_reserve(&a->slab_allocator);
}
static inline void lb_heap_cancel(lb_heap *a, int ticket) {
  if (ticket != -1)
    ct_arena_release(&a->slab_allocator, ticket);
}
static inline void lb_heap_release_prepared(lb_heap *a, uint64_t offset, size_t size, int ticket) {
  lb_heap_slot *head = &a->size_class_heads[lb_size_class(size)],
               *s = (lb_heap_slot *)ct_arena_at(&a->slab_allocator, ticket);
  s->file_offset = offset;
  s->next_slab_index = head->next_slab_index;
  head->next_slab_index = ticket;
}
static inline bool lb_heap_release(lb_heap *a, uint64_t offset, size_t size) {
  uint8_t cl = lb_size_class(size);
  int ticket;
  if (cl == LB_INVALID_CLASS || offset >= a->high_watermark ||
      lb_slot_size(cl) > a->high_watermark - offset)
    return false;
  ticket = lb_heap_prepare(a);
  if (ticket == -1)
    return false;
  lb_heap_release_prepared(a, offset, size, ticket);
  return true;
}
#endif
