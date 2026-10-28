#ifndef CT_BYTES_H
#define CT_BYTES_H
#include "array.h"
#include <stdint.h>
#include <string.h>
typedef struct ct_bytes {
  uint8_t *data;
  size_t size, capacity;
} ct_bytes;
static inline bool ct_bytes_append(ct_bytes *b, const void *p, size_t n) {
  if (b->size == SIZE_MAX || n > SIZE_MAX - b->size - 1 ||
      !ct_array_reserve(&b->data, &b->capacity, b->size + n + 1, 1))
    return false;
  if (n)
    memcpy(b->data + b->size, p, n);
  b->size += n;
  b->data[b->size] = 0;
  return true;
}
/* Input must not alias the buffer. Failure preserves its contents. */
static inline bool ct_bytes_set(ct_bytes *b, const char *p, size_t n) {
  if (n == SIZE_MAX || !ct_array_reserve(&b->data, &b->capacity, n + 1, 1))
    return false;
  if (n)
    memcpy(b->data, p, n);
  b->size = n;
  b->data[n] = 0;
  return true;
}
static inline const char *ct_bytes_string(const ct_bytes *b) {
  return b->data ? (const char *)b->data : "";
}
static inline void ct_bytes_free(ct_bytes *b) {
  free(b->data);
  memset(b, 0, sizeof(*b));
}
#endif
