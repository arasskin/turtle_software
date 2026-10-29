#ifndef LB_OP_1_CONTINUE_RECONCILIATION_H
#define LB_OP_1_CONTINUE_RECONCILIATION_H
#include "models.h"
#include "namespaces.h"
#include "exchange.h"
#include "index_sync.h"
#include "owner_authorization.h"
#include "request_lifetime.h"
#include "subscriber_delivery.h"
/* Operation 1: compare fingerprints; issue an owner challenge only on a
 * differing exchange, preserving the equal-state optimization. */
static inline void lb_continue_reconciliation_reject(lb_task *t) {
  if (!lb_task_stop(t, lb_abort_reject)) return;
  lb_reply_rejected(&t->reply);
  t->phase = lb_task_done;
}
static inline bool lb_continue_reconciliation_restart_reply(lb_task *t, const lb_directory *namespaces, uint64_t epoch) {
  lb_namespace_entry *entry = lb_directory_entry(namespaces, t->ns);
  bool busy = entry && entry->writing;
  lb_reply_header(&t->reply, entry ? lb_status_retry : lb_status_absent, epoch,
                  entry ? entry->revision : 0);
  if (entry && !busy && !lb_exchange_begin(lb_index_source(&entry->value), &t->reply)) return false;
  return !t->reply.failed;
}
static inline void lb_continue_reconciliation_restart(lb_task *t, const lb_directory *namespaces, uint64_t epoch) {
  if (!lb_task_stop(t, lb_abort_restart)) return;
  if (!lb_continue_reconciliation_restart_reply(t, namespaces, epoch)) { lb_continue_reconciliation_reject(t); return; }
  t->phase = lb_task_done;
}
static inline void lb_continue_reconciliation_resume(lb_task *t, lb_directory *namespaces, uint64_t epoch, size_t budget) {
  if (t->phase == lb_task_done) return;
  if (t->abort_status == lb_abort_reject) { lb_continue_reconciliation_reject(t); return; }
  if (t->abort_status == lb_abort_restart) { lb_continue_reconciliation_restart(t, namespaces, epoch); return; }
  t->waiting_capacity = false;
  (void)budget;
  uint64_t version = lb_namespace_revision(namespaces, t->ns);
  if (lb_namespace_writing(namespaces, t->ns)) {
    lb_reply_header(&t->reply, lb_status_retry, epoch, version);
    t->phase = lb_task_done;
    return;
  }
  if (t->epoch != epoch || t->version != version) { lb_continue_reconciliation_restart(t, namespaces, epoch); return; }
  lb_buffer compared = CT_ZERO;
  lb_index empty = CT_ZERO;
  lb_index *tree = lb_directory_get(namespaces, t->ns);
  lb_reply_header(&t->reply, lb_status_ok, epoch, version);
  if (!lb_exchange_receive(lb_index_source(tree ? tree : &empty), lb_buffer_bytes(&t->payload),
                           &compared) || !lb_append(&t->reply, compared.data, compared.size))
    lb_continue_reconciliation_reject(t);
  else t->phase = lb_task_done;
  lb_buffer_free(&compared);
}
static inline bool lb_continue_reconciliation_begin(lb_task *t, channel *ch, const lb_request *request) {
  if (!ch) return false;
  bool ok = lb_task_prepare(t, lb_op_continue_reconciliation, lb_name(ch->name),
                            request->epoch, request->revision, request->payload);
  return ok;
}
static inline bool lb_continue_reconciliation_finish(lb_task *t, channel *ch, lb_subscribers *subscribers) {
  lb_auth *auth = ch ? &ch->auth : NULL;
  if (!lb_reply_valid(&t->reply)) return false;
  if (t->reply.data[0] == lb_status_ok && auth && auth->pushing && !auth->issued &&
      !lb_exchange_equal(lb_buffer_bytes(&t->payload), lb_bytes_make(t->reply.data + 17, t->reply.size - 17))) {
    if (!lb_buffer_reserve(&t->reply, t->reply.size + 32)) lb_continue_reconciliation_reject(t);
    else {
      lb_auth_challenge(auth);
      memmove(t->reply.data + 49, t->reply.data + 17, t->reply.size - 17);
      memcpy(t->reply.data + 17, auth->challenge, 32);
      t->reply.size += 32;
      t->reply.data[0] |= LB_CHALLENGE_FLAG;
    }
  }
  if (!lb_reply_valid(&t->reply)) return false;
  uint8_t status = t->reply.data[0] & 0x7f;
  if (status == lb_status_absent || (status == lb_status_retry && t->reply.size > 17))
    subscription_reset_revision(subscribers, ch,
                                lb_integer(lb_bytes_make(t->reply.data + 9, 8)));
  return true;
}
#endif
