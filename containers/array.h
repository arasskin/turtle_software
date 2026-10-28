#ifndef CT_ARRAY_H
#define CT_ARRAY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Grow a typed pointer + element capacity, without aliasing it through void **.
 * Zero-initialize both before first use; free the pointer when done. Growth may
 * move elements: keep indices, not pointers, across reserve. New elements are
 * uninitialized. Failure leaves both pointer and capacity unchanged.
 * limit is an element count, not a byte count. */
static inline bool ct_array_reserve_limit(void *address, size_t *capacity, size_t count,
                                          size_t item, size_t limit) {
  size_t cap, maximum;
  void *data, *next;
  if (!item || count > SIZE_MAX / item || count > limit)
    return false;
  if (count <= *capacity)
    return true;
  maximum = SIZE_MAX / item;
  if (limit < maximum)
    maximum = limit;
  cap = *capacity ? *capacity : (maximum < 8 ? maximum : 8);
  while (cap < count) {
    if (cap > maximum / 2) {
      cap = count;
      break;
    }
    cap *= 2;
  }
  memcpy(&data, address, sizeof(data));
  next = realloc(data, cap * item);
  if (!next)
    return false;
  memcpy(address, &next, sizeof(next));
  *capacity = cap;
  return true;
}
static inline bool ct_array_reserve(void *address, size_t *capacity, size_t count, size_t item) {
  return ct_array_reserve_limit(address, capacity, count, item, SIZE_MAX);
}
#endif
