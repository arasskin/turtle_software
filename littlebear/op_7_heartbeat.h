#ifndef LB_OP_7_HEARTBEAT_H
#define LB_OP_7_HEARTBEAT_H
#include "models.h"
#include "codec.h"
#include "request_lifetime.h"
/* Operation 7 is connection-scoped: no channel, namespace or owner checks. */
static inline bool lb_heartbeat_begin(lb_task *t) {
  lb_buffer reply = CT_ZERO;
  lb_reply_header(&reply, lb_status_ok, 0, 0);
  lb_task_ready(t, &reply);
  t->operation = lb_op_heartbeat;
  return true;
}
static inline bool lb_heartbeat_finish(lb_task *t) {
  return lb_reply_valid(&t->reply);
}
#endif
