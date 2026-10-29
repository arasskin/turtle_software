#ifndef LB_OP_5_CLOSE_CHANNEL_H
#define LB_OP_5_CLOSE_CHANNEL_H
#include "namespace_channels.h"
#include "models.h"
#include "subscriber_delivery.h"
#include "request_lifetime.h"
/* Operation 5: release the subscription and its namespace-bound channel. */
static inline bool lb_close_channel_begin(lb_task *t,
                                         const lb_request *request) {
  lb_buffer reply = CT_ZERO;
  lb_reply_header(&reply, lb_status_ok, request->epoch, request->revision);
  lb_task_ready(t, &reply);
  t->operation = lb_op_close_channel;
  return true;
}
static inline bool lb_close_channel_finish(lb_task *t, lb_peer *peer, lb_subscribers *subscribers) {
  if (!lb_reply_valid(&t->reply)) return false;
  if ((t->reply.data[0] & 0x7f) == lb_status_ok) {
    channel *ch = namespace_channel(peer);
    subscription_remove(subscribers, ch->subscription);
    namespace_channel_release(peer);
  }
  return true;
}
#endif
