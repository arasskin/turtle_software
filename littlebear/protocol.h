#ifndef LB_PROTOCOL_H
#define LB_PROTOCOL_H
#include "codec.h"
typedef struct lb_request {
  uint32_t id, channel;
  uint8_t op;
  lb_bytes public_key, payload;
  uint64_t epoch, revision;
  lb_bytes challenge, signature, signed_bytes;
  uint64_t sequence;
} lb_request;
static inline bool lb_parse(lb_bytes packet, lb_bytes schema, lb_request *out) {
  lb_reader r = {packet, false};
  memset(out, 0, sizeof(*out));
  if (!lb_bytes_equal(lb_take(&r, 4), lb_bytes_make("LBS\5", 4)))
    return false;
  out->id = lb_read_u32(&r);
  out->channel = lb_read_u32(&r);
  out->op = lb_read_u8(&r);
  if (!out->id || out->op > 8)
    return false;
  out->public_key = lb_read_blob(&r);
  if (!lb_bytes_equal(lb_read_blob(&r), schema))
    return false;
  if (out->op == 7) {
    if (out->channel || out->public_key.size)
      return false;
  } else if (!out->channel || out->public_key.size != 32)
    return false;
  out->epoch = lb_read_u64(&r);
  out->revision = lb_read_u64(&r);
  out->payload = lb_read_blob(&r);
  if (out->op == 3 || out->op == 6) {
    out->challenge = lb_take(&r, 32);
    out->sequence = lb_read_u64(&r);
    out->signature = lb_take(&r, 64);
    if (packet.size < 64)
      return false;
    out->signed_bytes = lb_bytes_make(packet.data, packet.size - 64);
  }
  if (!lb_reader_end(&r))
    return false;
  if ((out->op == 4 || out->op == 5 || out->op >= 7) && out->payload.size)
    return false;
  return true;
}
#endif
