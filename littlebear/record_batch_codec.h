#ifndef LB_RECORD_BATCH_CODEC_H
#define LB_RECORD_BATCH_CODEC_H
#include "codec.h"
#include "index.h"
typedef struct lb_change {
  uint64_t id;
  lb_bytes value;
  bool present;
} lb_change;
typedef struct lb_record_batch {
  lb_change *changes;
  size_t count, position;
  lb_bytes records_payload; /* borrows the request's copied payload */
} lb_record_batch;
static inline int lb_change_order(const void *a, const void *b) {
  uint64_t x = ((const lb_change *)a)->id, y = ((const lb_change *)b)->id;
  return (x > y) - (x < y);
}
static inline bool lb_decode_record_batch(lb_record_batch *t, lb_bytes payload) {
  lb_reader r = {payload, false};
  uint32_t count;
  size_t i;
  t->records_payload = r.data;
  count = lb_read_u32(&r);
  if (r.failed || count > LB_MAX_RECORDS || count > r.data.size / 9)
    return false;
  if (count) {
    t->changes = (lb_change *)calloc(count, sizeof(*t->changes));
    if (!t->changes)
      return false;
  }
  t->count = count;
  for (i = 0; i < count; ++i) {
    lb_change *c = &t->changes[i];
    c->id = lb_read_u64(&r);
    uint8_t present = lb_read_u8(&r);
    if (present > 1)
      return false;
    c->present = present != 0;
    if (c->present) {
      uint8_t key[8];
      unsigned j;
      XXH64_state_t hash;
      c->value = lb_read_blob(&r);
      if (c->value.size < 8 || c->value.size > 65535)
        return false;
      for (j = 0; j < 8; ++j)
        key[j] = (uint8_t)(c->id >> (56 - 8 * j));
      XXH64_reset(&hash, 0);
      XXH64_update(&hash, key, 8);
      XXH64_update(&hash, c->value.data + 8, c->value.size - 8);
      if (lb_integer(c->value) != XXH64_digest(&hash))
        return false;
    }
  }
  if (!lb_reader_end(&r))
    return false;
  if (count > 1)
    qsort(t->changes, count, sizeof(*t->changes), lb_change_order);
  for (i = 1; i < count; ++i)
    if (t->changes[i - 1].id == t->changes[i].id)
      return false;
  return true;
}
#endif
