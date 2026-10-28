#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int allocation_limit = -1;
static int fail(void) {
  if (allocation_limit < 0)
    return 0;
  return allocation_limit-- == 0;
}
static void *test_malloc(size_t n) { return fail() ? NULL : malloc(n); }
static void *test_calloc(size_t n, size_t size) { return fail() ? NULL : calloc(n, size); }
static void *test_realloc(void *p, size_t n) { return fail() ? NULL : realloc(p, n); }
#define malloc test_malloc
#define calloc test_calloc
#define realloc test_realloc
#include "../arena.h"
#include "../array.h"
#include "../buffer.h"
#include "../bytes.h"
#include "../map.h"
#include "../pool.h"
#include "../tree.h"
#undef malloc
#undef calloc
#undef realloc
static void arrays(void) {
  uint64_t *items = NULL;
  size_t capacity = 0;
  for (size_t i = 0; i < 4097; ++i) {
    assert(ct_array_reserve(&items, &capacity, i + 1, sizeof(*items)));
    items[i] = i * 17;
  }
  for (size_t i = 0; i < 4097; ++i)
    assert(items[i] == i * 17);
  uint64_t *before = items;
  size_t old_capacity = capacity;
  allocation_limit = 0;
  assert(!ct_array_reserve(&items, &capacity, capacity + 1, sizeof(*items)));
  assert(items == before && capacity == old_capacity && items[4096] == 4096 * 17);
  assert(!ct_array_reserve(&items, &capacity, SIZE_MAX, sizeof(*items)));
  assert(!ct_array_reserve_limit(&items, &capacity, 5, sizeof(*items), 4));
  assert(!ct_array_reserve(&items, &capacity, 1, 0));
  free(items);
}
static void buffers(void) {
  ct_buffer b = {0};
  const char text[] = "bytes";
  assert(ct_buffer_append_limit(&b, text, 5, 5));
  assert(b.size == 5 && b.capacity == 5);
  assert(!ct_buffer_append_limit(&b, text, 1, 5));
  assert(b.failed && b.size == 5 && !memcmp(b.data, text, 5));
  assert(!ct_buffer_append(&b, text, 1));
  ct_buffer_clear(&b);
  allocation_limit = 0;
  assert(!ct_buffer_reserve(&b, 100));
  assert(b.failed && b.capacity == 5 && b.size == 0);
  ct_buffer_clear(&b);
  assert(ct_buffer_append(&b, text, 5));
  assert(!ct_buffer_append(&b, text, SIZE_MAX));
  assert(b.size == 5);
  ct_buffer_free(&b);
  assert(!b.data && !b.capacity && !b.size && !b.failed);
}
static void maps(void) {
  ct_map m = {0};
  for (uint64_t i = 0; i < 8; ++i)
    assert(ct_map_put(&m, i, (uintptr_t)i));
  for (int failure = 0; failure < 2; ++failure) {
    allocation_limit = failure;
    assert(!ct_map_put(&m, UINT64_MAX, 99));
    allocation_limit = -1;
    assert(m.size == 8 && !ct_map_get(&m, UINT64_MAX));
    for (uint64_t i = 0; i < 8; ++i)
      assert(ct_map_get(&m, i)->value == i);
  }
  assert(ct_map_put(&m, UINT64_MAX, 99));
  assert(ct_map_erase(&m, 0));
  assert(ct_map_get(&m, UINT64_MAX)->value == 99);
  assert(!ct_map_reserve(&m, SIZE_MAX));
  ct_map_free(&m);
}
static void bytes(void) {
  ct_bytes b = {0};
  assert(ct_bytes_set(&b, "abc", 3));
  assert(ct_bytes_append(&b, "def", 3) && !strcmp(ct_bytes_string(&b), "abcdef"));
  char large[1024] = {0};
  allocation_limit = 0;
  assert(!ct_bytes_set(&b, large, sizeof(large)));
  allocation_limit = -1;
  assert(b.size == 6 && !strcmp(ct_bytes_string(&b), "abcdef"));
  assert(!ct_bytes_append(&b, "", SIZE_MAX));
  assert(b.size == 6 && !ct_bytes_set(&b, "", SIZE_MAX));
  assert(ct_bytes_set(&b, "", 0) && !b.size && !*ct_bytes_string(&b));
  ct_bytes_free(&b);
}
int main(void) {
  bytes();
  arrays();
  buffers();
  maps();
  puts("PASS containers: growth, overflow, allocation rollback, bounded buffers, map relocation");
  return 0;
}
