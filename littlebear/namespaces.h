#ifndef LB_NAMESPACES_H
#define LB_NAMESPACES_H
#include "index.h"
#include "codec.h"
typedef XXH128_hash_t lb_namespace;
/* Dense entries with 32-bit linear-probing indices. Deletion backshifts the
 * index and moves the last value. Headers must be looked up after mutation. */
typedef struct lb_namespace_entry {
  lb_namespace key;
  lb_index value;
  uint64_t revision;
  bool writing;
} lb_namespace_entry;
typedef struct lb_directory {
  lb_namespace_entry *entries;
  uint32_t *slots;
  size_t count, capacity, buckets;
  ct_allocate_fn allocate;
  bool initialized;
} lb_directory;
static inline bool lb_namespace_equal(lb_namespace a, lb_namespace b) {
  return a.low64 == b.low64 && a.high64 == b.high64;
}
static inline bool lb_directory_valid(size_t n) {
  return n < UINT32_MAX && n <= SIZE_MAX / sizeof(lb_namespace_entry);
}
static inline size_t lb_directory_slot(const lb_directory *t, lb_namespace k) {
  size_t i;
  if (!t->buckets)
    return SIZE_MAX;
  i = (size_t)k.low64 & (t->buckets - 1);
  while (t->slots[i]) {
    if (lb_namespace_equal(t->entries[t->slots[i] - 1].key, k))
      return i;
    i = (i + 1) & (t->buckets - 1);
  }
  return i;
}
static inline lb_index *lb_directory_get(lb_directory *t, lb_namespace k) {
  size_t i = lb_directory_slot(t, k);
  return i == SIZE_MAX || !t->slots[i] ? NULL : &t->entries[t->slots[i] - 1].value;
}
static inline lb_namespace_entry *lb_directory_entry(lb_directory *t, lb_namespace k) {
  size_t i = lb_directory_slot(t, k);
  return i == SIZE_MAX || !t->slots[i] ? NULL : &t->entries[t->slots[i] - 1];
}
static inline bool lb_directory_reserve(lb_directory *t, size_t capacity) {
  size_t buckets = 8, i;
  lb_namespace_entry *entries = NULL;
  uint32_t *slots = NULL;
  if (!t->initialized || !lb_directory_valid(capacity))
    return false;
  if (capacity <= t->capacity)
    return true;
  while (capacity > buckets - buckets / 4) {
    if (buckets > SIZE_MAX / 2 / sizeof(uint32_t))
      return false;
    buckets *= 2;
  }
  entries = (lb_namespace_entry *)t->allocate(sizeof(void *), capacity * sizeof(*entries));
  if (!entries)
    return false;
  if (t->count)
    memcpy(entries, t->entries, t->count * sizeof(*entries));
  if (buckets != t->buckets) {
    slots = (uint32_t *)t->allocate(sizeof(void *), buckets * sizeof(*slots));
    if (!slots) {
      free(entries);
      return false;
    }
    memset(slots, 0, buckets * sizeof(*slots));
    for (i = 0; i < t->count; ++i) {
      size_t j = (size_t)entries[i].key.low64 & (buckets - 1);
      while (slots[j])
        j = (j + 1) & (buckets - 1);
      slots[j] = (uint32_t)(i + 1);
    }
    free(t->slots);
    t->slots = slots;
    t->buckets = buckets;
  }
  free(t->entries);
  t->entries = entries;
  t->capacity = capacity;
  return true;
}
static inline lb_directory lb_directory_create(size_t capacity, ct_allocate_fn fn) {
  lb_directory t = CT_ZERO;
  if (!fn || !lb_directory_valid(capacity))
    return t;
  t.allocate = fn;
  t.initialized = true;
  if (capacity && !lb_directory_reserve(&t, capacity)) {
    free(t.entries);
    free(t.slots);
    memset(&t, 0, sizeof(t));
  }
  return t;
}
/* Callers destroy record trees before clearing or destroying the directory;
 * moving a header transfers its unique root. */
static inline void lb_directory_destroy(lb_directory *t) {
  free(t->entries);
  free(t->slots);
  memset(t, 0, sizeof(*t));
}
static inline void lb_directory_clear(lb_directory *t) {
  if (t->buckets)
    memset(t->slots, 0, t->buckets * sizeof(*t->slots));
  t->count = 0;
}
static inline bool lb_directory_insert(lb_directory *t, lb_namespace k, lb_index v) {
  size_t i;
  if (!t->initialized || lb_directory_get(t, k))
    return false;
  if (t->count == t->capacity) {
    size_t next = t->capacity ? t->capacity + t->capacity / 2 + 1 : 8;
    if (!lb_directory_reserve(t, next))
      return false;
  }
  i = lb_directory_slot(t, k);
  memset(&t->entries[t->count], 0, sizeof(*t->entries));
  t->entries[t->count].key = k;
  t->entries[t->count].value = v;
  t->slots[i] = (uint32_t)(++t->count);
  return true;
}
static inline bool lb_directory_erase(lb_directory *t, lb_namespace k) {
  size_t hole = lb_directory_slot(t, k), i, index, mask;
  if (hole == SIZE_MAX || !t->slots[hole])
    return false;
  index = t->slots[hole] - 1;
  mask = t->buckets - 1;
  i = (hole + 1) & mask;
  while (t->slots[i]) {
    size_t home = (size_t)t->entries[t->slots[i] - 1].key.low64 & mask;
    if (((i - home) & mask) >= ((i - hole) & mask)) {
      t->slots[hole] = t->slots[i];
      hole = i;
    }
    i = (i + 1) & mask;
  }
  t->slots[hole] = 0;
  --t->count;
  if (index != t->count) {
    lb_namespace_entry last = t->entries[t->count];
    i = lb_directory_slot(t, last.key);
    t->slots[i] = (uint32_t)(index + 1);
    t->entries[index] = last;
  }
  return true;
}
#endif
