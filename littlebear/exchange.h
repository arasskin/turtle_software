#ifndef LB_EXCHANGE_H
#define LB_EXCHANGE_H
#include "protocol.h"
#include "reconcile.h"
static inline bool lb_exchange_begin(lb_recon_source source, lb_buffer *reply) {
  lb_recon_workspace work;
  uint8_t bytes[4096];
  lb_recon_output out = {bytes, sizeof(bytes), 0, 0};
  if (lb_recon_begin(source, &work, &out) != lb_recon_status_complete)
    return false;
  return lb_append(reply, bytes, out.size);
}
static inline bool lb_exchange_receive(lb_recon_source source, lb_bytes packet, lb_buffer *reply) {
  lb_recon_workspace work;
  lb_recon_cursor cursor = CT_ZERO;
  uint8_t bytes[4096];
  uint64_t have[32], need[32];
  size_t turns;
  lb_recon_differences diff = {have, 32, 0, need, 32, 0};
  lb_recon_output out = {bytes, sizeof(bytes), 0, 0};
  lb_recon_bytes input = {packet.data, packet.size};
  if (!lb_append(reply, "LBR\3", 4))
    return false;
  for (turns = 0; turns <= LB_MAX_RECORDS; ++turns) {
    lb_recon_status rc =
        lb_recon_receive(source, lb_recon_role_responder, input, &cursor, &work, &out, &diff, 64);
    if (rc != lb_recon_status_complete && rc != lb_recon_status_more &&
        rc != lb_recon_status_output_full && rc != lb_recon_status_differences_full)
      return false;
    if (out.size > 4 && !lb_append(reply, bytes + 4, out.size - 4))
      return false;
    if (reply->size > LB_MAX_PACKET)
      return false;
    if (rc == lb_recon_status_complete)
      return true;
  }
  return false;
}
static inline bool lb_exchange_equal(lb_bytes request, lb_bytes reply) {
  if (lb_bytes_equal(reply, lb_bytes_make("LBR\3", 4)))
    return true;
  return request.size >= 7 && request.size == reply.size && !memcmp(request.data, "LBR\3\1\0", 6) &&
         !memcmp(reply.data, "LBR\3\2\0", 6) && request.data[6] <= lb_recon_LEAF_IDS &&
         request.size == 7 + (size_t)request.data[6] * 16 &&
         !memcmp(request.data + 6, reply.data + 6, request.size - 6);
}
#endif
