#ifndef CT_MAP_H
#define CT_MAP_H
#include "array.h"
/* Dense entries plus 32-bit bucket positions. Zero means empty, so all 64-bit
 * keys are valid. Erase backward-shifts buckets and swaps the last dense entry.
 * No tombstones, per-entry allocations, or sparse-array iteration. */
typedef struct ct_pair {
  uint64_t key;
  uintptr_t value;
} ct_pair;
typedef struct ct_map {
  ct_pair *items;
  uint32_t *slots;
  size_t size, capacity, buckets;
} ct_map;
static inline uint64_t ct_mix(uint64_t x) {
  x ^= x >> 30;
  x *= UINT64_C(0xbf58476d1ce4e5b9);
  x ^= x >> 27;
  x *= UINT64_C(0x94d049bb133111eb);
  return x ^ (x >> 31);
}
static inline size_t ct_map_slot(const ct_map *m, uint64_t key) {
  size_t s = (size_t)ct_mix(key) & (m->buckets - 1);
  while (m->slots[s] && m->items[m->slots[s] - 1].key != key)
    s = (s + 1) & (m->buckets - 1);
  return s;
}
static inline ct_pair *ct_map_get(const ct_map *m, uint64_t key) {
  if (!m->size)
    return NULL;
  uint32_t i = m->slots[ct_map_slot(m, key)];
  return i ? m->items + i - 1 : NULL;
}
static inline bool ct_map_reserve(ct_map *m, size_t n) {
  if (n <= m->capacity)
    return true;
  if (n >= UINT32_MAX)
    return false;
  size_t cap = m->capacity ? m->capacity : 8;
  while (cap < n) {
    if (cap > UINT32_MAX / 2) {
      cap = n;
      break;
    }
    cap *= 2;
  }
  size_t buckets = 16;
  while (buckets - buckets / 4 < cap) {
    if (buckets > SIZE_MAX / 2 / sizeof(uint32_t))
      return false;
    buckets *= 2;
  }
  if (cap > SIZE_MAX / sizeof(ct_pair) || buckets > SIZE_MAX / sizeof(uint32_t))
    return false;
  ct_pair *items = malloc(cap * sizeof(*items));
  uint32_t *slots = calloc(buckets, sizeof(*slots));
  if (!items || !slots) {
    free(items);
    free(slots);
    return false;
  }
  if (m->size)
    memcpy(items, m->items, m->size * sizeof(*items));
  ct_map next = {items, slots, m->size, cap, buckets};
  for (size_t i = 0; i < next.size; ++i)
    next.slots[ct_map_slot(&next, items[i].key)] = (uint32_t)i + 1;
  free(m->items);
  free(m->slots);
  *m = next;
  return true;
}
static inline bool ct_map_put(ct_map *m, uint64_t key, uintptr_t value) {
  ct_pair *p = ct_map_get(m, key);
  if (p) {
    p->value = value;
    return true;
  }
  if (!ct_map_reserve(m, m->size + 1))
    return false;
  size_t s = ct_map_slot(m, key);
  m->items[m->size] = (ct_pair){key, value};
  m->slots[s] = (uint32_t)++m->size;
  return true;
}
static inline bool ct_map_erase(ct_map *m, uint64_t key) {
  if (!m->size)
    return false;
  size_t hole = ct_map_slot(m, key);
  uint32_t at = m->slots[hole];
  if (!at)
    return false;
  size_t mask = m->buckets - 1, scan = (hole + 1) & mask;
  while (m->slots[scan]) {
    size_t home = (size_t)ct_mix(m->items[m->slots[scan] - 1].key) & mask;
    if (((scan - home) & mask) >= ((scan - hole) & mask)) {
      m->slots[hole] = m->slots[scan];
      hole = scan;
    }
    scan = (scan + 1) & mask;
  }
  m->slots[hole] = 0;
  --m->size;
  if (at - 1 != m->size) {
    ct_pair moved = m->items[m->size];
    size_t s = ct_map_slot(m, moved.key);
    m->items[at - 1] = moved;
    m->slots[s] = at;
  }
  return true;
}
static inline void ct_map_clear(ct_map *m) {
  m->size = 0;
  if (m->slots)
    memset(m->slots, 0, m->buckets * sizeof(*m->slots));
}
static inline void ct_map_free(ct_map *m) {
  free(m->items);
  free(m->slots);
  memset(m, 0, sizeof(*m));
}
static inline int ct_key_compare(const void *a, const void *b) {
  uint64_t x = ((const ct_pair *)a)->key, y = ((const ct_pair *)b)->key;
  return (x > y) - (x < y);
}
#endif
