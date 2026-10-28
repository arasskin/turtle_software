#ifndef CT_BUFFER_H
#define CT_BUFFER_H
#include "array.h"

/* Binary buffer with sticky allocation/overflow failure. Appends borrow input
 * outside the buffer (growth can relocate it). Clear retains capacity; free
 * releases it. Neither operation owns anything referenced by the bytes. */
typedef struct ct_buffer {
  uint8_t *data;
  size_t size, capacity;
  bool failed;
} ct_buffer;
static inline bool ct_buffer_reserve_limit(ct_buffer *b, size_t size, size_t limit) {
  if (b->failed)
    return false;
  if (size > limit) {
    b->failed = true;
    return false;
  }
  if (size && size < 64)
    size = limit < 64 ? limit : 64;
  if (!ct_array_reserve_limit(&b->data, &b->capacity, size, 1, limit)) {
    b->failed = true;
    return false;
  }
  return true;
}
static inline bool ct_buffer_reserve(ct_buffer *b, size_t size) {
  return ct_buffer_reserve_limit(b, size, SIZE_MAX);
}
static inline bool ct_buffer_append_limit(ct_buffer *b, const void *data, size_t size,
                                          size_t limit) {
  if (size > SIZE_MAX - b->size || !ct_buffer_reserve_limit(b, b->size + size, limit)) {
    b->failed = true;
    return false;
  }
  if (size)
    memcpy(b->data + b->size, data, size);
  b->size += size;
  return true;
}
static inline bool ct_buffer_append(ct_buffer *b, const void *data, size_t size) {
  return ct_buffer_append_limit(b, data, size, SIZE_MAX);
}
static inline void ct_buffer_clear(ct_buffer *b) {
  b->size = 0;
  b->failed = false;
}
static inline void ct_buffer_free(ct_buffer *b) {
  free(b->data);
  memset(b, 0, sizeof(*b));
}
#endif
