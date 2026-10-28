#ifndef LB_CODEC_H
#define LB_CODEC_H
#include "base.h"
#include "../../containers/buffer.h"
enum {
  LB_MAX_FRAME = 8 * 1024 * 1024,
  LB_MAX_PACKET = LB_MAX_FRAME - 2048,
  LB_MAX_RECORDS = 1000000
};
typedef struct lb_bytes {
  const uint8_t *data;
  size_t size;
} lb_bytes;
typedef ct_buffer lb_buffer;
typedef struct lb_reader {
  lb_bytes data;
  bool failed;
} lb_reader;
static inline lb_bytes lb_bytes_make(const void *p, size_t n) {
  lb_bytes b = {(const uint8_t *)p, n};
  return b;
}
static inline lb_bytes lb_buffer_bytes(const lb_buffer *b) {
  return lb_bytes_make(b->data, b->size);
}
static inline void lb_buffer_free(lb_buffer *b) { ct_buffer_free(b); }
static inline void lb_buffer_clear(lb_buffer *b) { ct_buffer_clear(b); }
static inline bool lb_buffer_reserve(lb_buffer *b, size_t n) {
  return ct_buffer_reserve_limit(b, n, LB_MAX_FRAME + 4);
}
static inline bool lb_append(lb_buffer *b, const void *p, size_t n) {
  return ct_buffer_append_limit(b, p, n, LB_MAX_FRAME + 4);
}
static inline void lb_u8(lb_buffer *b, uint8_t n) {
  lb_append(b, &n, 1);
}
static inline void lb_u32(lb_buffer *b, uint32_t n) {
  uint8_t bytes[4];
  unsigned i;
  for (i = 0; i < 4; ++i)
    bytes[i] = (uint8_t)(n >> (24 - i * 8));
  lb_append(b, bytes, 4);
}
static inline void lb_u64(lb_buffer *b, uint64_t n) {
  uint8_t bytes[8];
  unsigned i;
  for (i = 0; i < 8; ++i)
    bytes[i] = (uint8_t)(n >> (56 - i * 8));
  lb_append(b, bytes, 8);
}
static inline void lb_blob(lb_buffer *b, lb_bytes data) {
  if (data.size > UINT32_MAX) {
    b->failed = true;
    return;
  }
  lb_u32(b, (uint32_t)data.size);
  lb_append(b, data.data, data.size);
}
static inline lb_bytes lb_take(lb_reader *r, size_t n) {
  lb_bytes out = CT_ZERO;
  if (r->failed || n > r->data.size) {
    r->failed = true;
    return out;
  }
  out = lb_bytes_make(r->data.data, n);
  if (n)
    r->data.data += n;
  r->data.size -= n;
  return out;
}
static inline uint8_t lb_read_u8(lb_reader *r) {
  lb_bytes b = lb_take(r, 1);
  return b.size ? b.data[0] : 0;
}
static inline uint32_t lb_read_u32(lb_reader *r) {
  uint32_t n = 0;
  unsigned i;
  for (i = 0; i < 4; ++i)
    n = (n << 8) | lb_read_u8(r);
  return n;
}
static inline uint64_t lb_read_u64(lb_reader *r) {
  uint64_t n = 0;
  unsigned i;
  for (i = 0; i < 8; ++i)
    n = (n << 8) | lb_read_u8(r);
  return n;
}
static inline lb_bytes lb_read_blob(lb_reader *r) {
  uint32_t n = lb_read_u32(r);
  return lb_take(r, n);
}
static inline bool lb_reader_end(lb_reader *r) {
  return !r->failed && r->data.size == 0;
}
static inline bool lb_bytes_equal(lb_bytes a, lb_bytes b) {
  return a.size == b.size && (!a.size || memcmp(a.data, b.data, a.size) == 0);
}
static inline uint64_t lb_integer(lb_bytes b) {
  lb_reader r = {b, false};
  return lb_read_u64(&r);
}
#ifdef __cplusplus
#include "cpp/codec.h"
#endif
#endif
