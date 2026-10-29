#ifndef LB_OP_0_BEGIN_OWNER_SYNC_H
#define LB_OP_0_BEGIN_OWNER_SYNC_H
#include "models.h"
#include "namespaces.h"
#include "owner_authorization.h"
#include "request_lifetime.h"
#include "subscriber_delivery.h"
#include "exchange.h"
#include "index_sync.h"
/* Operation 0: begin a fresh owner sync with the first reconciliation packet.
 * The current server version anchors subsequent rounds; no prior version is required. */
static inline void lb_begin_owner_sync_reject(lb_task *t) {
  if (!lb_task_stop(t, lb_abort_reject)) return;
  lb_reply_rejected(&t->reply);
  t->phase = lb_task_done;
}
static inline void lb_begin_owner_sync_resume(lb_task *t, lb_directory *namespaces, uint64_t epoch, size_t budget) {
  if (t->phase == lb_task_done) return;
  if (t->abort_status == lb_abort_reject) { lb_begin_owner_sync_reject(t); return; }
  t->waiting_capacity = false;
  (void)budget;
  uint64_t version = lb_namespace_revision(namespaces, t->ns);
  bool absent = !lb_directory_entry(namespaces, t->ns);
  if (lb_namespace_writing(namespaces, t->ns)) {
    lb_reply_header(&t->reply, lb_status_retry, epoch, version);
  } else {
    lb_buffer compared = CT_ZERO;
    lb_index empty = CT_ZERO;
    lb_index *tree = lb_directory_get(namespaces, t->ns);
    lb_reply_header(&t->reply, absent ? lb_status_absent : lb_status_ok, epoch, version);
    if (!lb_exchange_receive(lb_index_source(tree ? tree : &empty), lb_buffer_bytes(&t->payload),
                             &compared) || !lb_append(&t->reply, compared.data, compared.size)) {
      lb_buffer_free(&compared);
      lb_begin_owner_sync_reject(t);
      return;
    }
    lb_buffer_free(&compared);
  }
  t->phase = lb_task_done;
}
static inline bool lb_begin_owner_sync_begin(lb_task *t, channel *ch, const lb_request *request) {
  if (!ch) return false;
  bool ok = lb_task_prepare(t, lb_op_begin_owner_sync, lb_name(ch->name),
                            request->epoch, request->revision, request->payload);
  return ok;
}
static inline bool lb_begin_owner_sync_finish(lb_task *t, channel *ch, lb_subscribers *subscribers) {
  if (!lb_reply_valid(&t->reply)) return false;
  uint8_t status = t->reply.data[0];
  if (status == lb_status_ok || status == lb_status_absent) {
    ch->needs_reconcile = false;
    memset(&ch->auth, 0, sizeof(ch->auth));
    ch->auth.pushing = true;
    bool needs_upload = status == lb_status_absent ||
        !lb_exchange_equal(lb_buffer_bytes(&t->payload), lb_bytes_make(t->reply.data + 17, t->reply.size - 17));
    if (needs_upload) {
      if (!lb_buffer_reserve(&t->reply, t->reply.size + 32)) lb_begin_owner_sync_reject(t);
      else {
        lb_auth_challenge(&ch->auth);
        memmove(t->reply.data + 49, t->reply.data + 17, t->reply.size - 17);
        memcpy(t->reply.data + 17, ch->auth.challenge, 32);
        t->reply.size += 32;
        t->reply.data[0] |= LB_CHALLENGE_FLAG;
      }
    }
  }
  if (!lb_reply_valid(&t->reply)) return false;
  if ((t->reply.data[0] & 0x7f) == lb_status_absent)
    subscription_reset_revision(subscribers, ch,
                                lb_integer(lb_bytes_make(t->reply.data + 9, 8)));
  return true;
}
#endif
