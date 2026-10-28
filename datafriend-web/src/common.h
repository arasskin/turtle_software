#ifndef DF_COMMON_H
#define DF_COMMON_H
#include "datafriend/c_api.h"
#include "datafriend/schema.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#define XXH_INLINE_ALL
#include "xxhash.h"
#ifdef DATAFRIEND_TESTING
void *df_memory_alloc(size_t);
void *df_memory_calloc(size_t, size_t);
void *df_memory_realloc(void *, size_t);
#define malloc df_memory_alloc
#define calloc df_memory_calloc
#define realloc df_memory_realloc
#endif
#include "../../containers/map.h"
#include "../../containers/buffer.h"
typedef struct df_bytes {
  const uint8_t *data;
  size_t size;
} df_bytes;
typedef ct_buffer df_writer;
typedef struct df_reader {
  df_bytes data;
  bool failed;
} df_reader;
static inline df_bytes df_span(const void *p, size_t n) {
  df_bytes b = {(const uint8_t *)p, n};
  return b;
}
static inline df_bytes df_text(const char *s) {
  return df_span(s, strlen(s));
}
static inline bool df_equal(df_bytes a, df_bytes b) {
  return a.size == b.size && (!a.size || !memcmp(a.data, b.data, a.size));
}
static inline int df_compare(df_bytes a, df_bytes b) {
  size_t n = a.size < b.size ? a.size : b.size;
  int c = n ? memcmp(a.data, b.data, n) : 0;
  return c ? c : (a.size > b.size) - (a.size < b.size);
}
static inline uint64_t df_hash(df_bytes b) {
  return XXH64(b.data, b.size, 0);
}
static inline bool df_reserve(df_writer *w, size_t n) {
  return ct_buffer_reserve(w, n);
}
static inline void df_append(df_writer *w, const void *p, size_t n) {
  ct_buffer_append(w, p, n);
}
static inline void df_u8(df_writer *w, uint8_t x) {
  df_append(w, &x, 1);
}
static inline void df_store64(uint8_t p[8], uint64_t x) {
  for (unsigned i = 0; i < 8; ++i)
    p[i] = (uint8_t)(x >> (56 - 8 * i));
}
static inline void df_u64(df_writer *w, uint64_t x) {
  uint8_t p[8];
  df_store64(p, x);
  df_append(w, p, 8);
}
static inline void df_u32(df_writer *w, uint32_t x) {
  uint8_t p[4];
  for (unsigned i = 0; i < 4; ++i)
    p[i] = (uint8_t)(x >> (24 - 8 * i));
  df_append(w, p, 4);
}
static inline void df_blob(df_writer *w, df_bytes b) {
  if (b.size > UINT32_MAX) {
    w->failed = true;
    return;
  }
  df_u32(w, (uint32_t)b.size);
  df_append(w, b.data, b.size);
}
static inline df_bytes df_written(const df_writer *w) {
  return df_span(w->data, w->size);
}
static inline void df_writer_free(df_writer *w) {
  ct_buffer_free(w);
}
static inline df_bytes df_take(df_reader *r, size_t n) {
  if (r->failed || n > r->data.size) {
    r->failed = true;
    return df_span(NULL, 0);
  }
  df_bytes b = df_span(r->data.data, n);
  if (n)
    r->data.data += n;
  r->data.size -= n;
  return b;
}
static inline uint8_t df_read8(df_reader *r) {
  df_bytes b = df_take(r, 1);
  return b.size ? b.data[0] : 0;
}
static inline uint32_t df_read32(df_reader *r) {
  uint32_t n = 0;
  for (unsigned i = 0; i < 4; ++i)
    n = n << 8 | df_read8(r);
  return n;
}
static inline uint64_t df_read64(df_reader *r) {
  uint64_t n = 0;
  for (unsigned i = 0; i < 8; ++i)
    n = n << 8 | df_read8(r);
  return n;
}
static inline uint64_t df_integer(df_bytes b) {
  df_reader r = {b, false};
  return df_read64(&r);
}
static inline df_bytes df_read_blob(df_reader *r) {
  uint32_t n = df_read32(r);
  return df_take(r, n);
}
static inline uint32_t df_count(df_reader *r, size_t min) {
  uint32_t n = df_read32(r);
  if (n > r->data.size / min) {
    r->failed = true;
    return 0;
  }
  return n;
}
static inline bool df_end(df_reader *r) {
  return !r->failed && !r->data.size;
}
static inline void df_error(df_buffer *out, const char *message) {
  out->size = strlen(message);
  out->data = malloc(out->size);
  if (out->data)
    memcpy(out->data, message, out->size);
  else
    out->size = 0;
}
static inline int df_reply(df_writer *w, df_buffer *out) {
  if (w->failed) {
    df_writer_free(w);
    df_error(out, "out of memory");
    return 1;
  }
  out->data = w->data;
  out->size = w->size;
  memset(w, 0, sizeof(*w));
  return 0;
}
#endif
