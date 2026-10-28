/* Failure injection is compiled only into test builds. Production uses libc
 * directly; there is no allocator dispatch or synchronization on its hot path. */
#include "datafriend/core.h"
#ifdef DATAFRIEND_TESTING
#include <pthread.h>
#include <stdlib.h>
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static int64_t remaining = -1;
void df_test_allocation_failure(int64_t after) {
  pthread_mutex_lock(&mutex);
  remaining = after;
  pthread_mutex_unlock(&mutex);
}
static int fail(void) {
  pthread_mutex_lock(&mutex);
  int result = remaining == 0;
  if (remaining >= 0)
    --remaining;
  pthread_mutex_unlock(&mutex);
  return result;
}
void *df_memory_alloc(size_t size) {
  return fail() ? NULL : malloc(size);
}
void *df_memory_calloc(size_t count, size_t size) {
  return fail() ? NULL : calloc(count, size);
}
void *df_memory_realloc(void *p, size_t size) {
  return fail() ? NULL : realloc(p, size);
}
#endif
