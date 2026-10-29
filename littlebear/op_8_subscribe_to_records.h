#ifndef LB_OP_8_SUBSCRIBE_TO_RECORDS_H
#define LB_OP_8_SUBSCRIBE_TO_RECORDS_H
#include "namespace_channels.h"
#include "models.h"
#include "namespaces.h"
#include "exchange.h"
#include "index_sync.h"
#include "subscriber_delivery.h"
#include "request_lifetime.h"
/* Operation 8: subscribe with a one-byte delivery mode:
 * 0 = reconciliation notifications; 1 = record batches with reconciliation fallback. */
static inline bool lb_subscribe_to_records_begin(lb_task *t, const lb_directory *namespaces, uint64_t epoch, channel *ch,
                                                        const lb_request *request) {
  lb_buffer reply = CT_ZERO;
  if (request->payload.size != 1 || request->payload.data[0] > 1) return false;
  lb_namespace_entry *entry = lb_directory_entry(namespaces, lb_name(ch->name));
  bool busy = entry && entry->writing;
  lb_reply_header(&reply, busy ? lb_status_retry : entry ? lb_status_ok : lb_status_absent, epoch, entry ? entry->revision : 0);
  bool ok = !reply.failed;
  if (entry && !busy) ok = ok && lb_exchange_begin(lb_index_source(&entry->value), &reply);
  lb_task_ready(t, &reply);
  t->operation = lb_op_subscribe_to_records;
  return ok && lb_append(&t->payload, request->payload.data, request->payload.size);
}
static inline bool lb_subscribe_to_records_finish(lb_task *t, lb_peer *peer, lb_subscribers *subscribers) {
  channel *ch = namespace_channel(peer);
  if (!lb_reply_valid(&t->reply)) return false;
  /* A rejected mode must not advance the subscriber's announced revision. */
  if ((t->reply.data[0] & 0x7f) == lb_status_rejected) return true;
  ch->subscribed = true;
  ch->optimistic = t->payload.data[0] == 1;
  if (!subscription_add(subscribers, peer, ch)) return false;
  uint8_t status = t->reply.data[0] & 0x7f;
  if (status == lb_status_absent || (status == lb_status_ok && t->reply.size > 17))
    subscription_reset_revision(subscribers, ch,
                                lb_integer(lb_bytes_make(t->reply.data + 9, 8)));
  return true;
}
#endif
