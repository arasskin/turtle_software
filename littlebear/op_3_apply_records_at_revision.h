#ifndef LB_OP_3_APPLY_RECORDS_AT_REVISION_H
#define LB_OP_3_APPLY_RECORDS_AT_REVISION_H
#include "owner_authorization.h"
#include "models.h"
#include "namespaces.h"
#include "exchange.h"
#include "index_sync.h"
#include "write_record_batch.h"
#include "record_batch_codec.h"
#include "request_lifetime.h"
#include "subscriber_delivery.h"
/* Operation 3: authorize once, require the supplied revision, apply the batch,
 * publish only after unlock, and acknowledge completion. */
typedef struct lb_apply_records_at_revision_state {
  lb_record_batch batch;
  bool writable, locked, succeeded;
  uint64_t before;
  size_t write_limit;
} lb_apply_records_at_revision_state;
static inline bool lb_apply_records_at_revision_prepare(lb_task *t, lb_namespace ns, bool writable,
                                    uint64_t epoch, uint64_t revision, lb_bytes payload,
                                    size_t write_limit) {
  if (!lb_task_prepare(t, lb_op_apply_records_at_revision, ns, epoch, revision, payload) ||
      !lb_task_allocate(t, sizeof(lb_apply_records_at_revision_state))) return false;
  lb_apply_records_at_revision_state *w = (lb_apply_records_at_revision_state *)t->state;
  w->writable = writable;
  w->write_limit = write_limit;
  return true;
}
/* Called after submitted I/O has drained, on success, rejection or disconnect. */
static inline void lb_apply_records_at_revision_release(lb_task *t, lb_directory *namespaces, size_t *active_writers) {
  lb_apply_records_at_revision_state *w = (lb_apply_records_at_revision_state *)t->state;
  if (!w || !w->locked) return;
  lb_namespace_entry *entry = lb_directory_entry(namespaces, t->ns);
  assert(entry && entry->writing && *active_writers);
  entry->writing = false;
  w->locked = false;
  --*active_writers;
  t->changed = true;
}
static inline void lb_apply_records_at_revision_dispose(lb_task *t) {
  lb_apply_records_at_revision_state *w = (lb_apply_records_at_revision_state *)t->state;
  if (w) {
    assert(!w->locked);
    if (!lb_task_drain(t)) abort();
    free(w->batch.changes);
  }
  lb_task_dispose(t);
}
static inline void lb_apply_records_at_revision_reject(lb_task *t, lb_directory *namespaces, size_t *active_writers) {
  if (!lb_task_stop(t, lb_abort_reject)) return;
  lb_reply_rejected(&t->reply);
  t->phase = lb_task_done;
  lb_apply_records_at_revision_release(t, namespaces, active_writers);
}
static inline bool lb_apply_records_at_revision_restart_reply(lb_task *t, const lb_directory *namespaces, uint64_t epoch) {
  lb_namespace_entry *entry = lb_directory_entry(namespaces, t->ns);
  bool busy = entry && entry->writing;
  lb_reply_header(&t->reply, entry ? lb_status_retry : lb_status_absent, epoch,
                  entry ? entry->revision : 0);
  if (entry && !busy && !lb_exchange_begin(lb_index_source(&entry->value), &t->reply)) return false;
  return !t->reply.failed;
}
static inline void lb_apply_records_at_revision_restart(lb_task *t, lb_directory *namespaces,
                                       uint64_t epoch, size_t *active_writers) {
  if (!lb_task_stop(t, lb_abort_restart)) return;
  if (!lb_apply_records_at_revision_restart_reply(t, namespaces, epoch)) {
    lb_apply_records_at_revision_reject(t, namespaces, active_writers);
    return;
  }
  t->phase = lb_task_done;
  lb_apply_records_at_revision_release(t, namespaces, active_writers);
}
static inline void lb_apply_records_at_revision_step(lb_task *t, lb_directory *namespaces, uint64_t epoch, lb_store *records, size_t *active_writers, size_t budget) {
  lb_apply_records_at_revision_state *w = (lb_apply_records_at_revision_state *)t->state;
  if (t->phase == lb_task_done) return;
  if (t->abort_status == lb_abort_reject) { lb_apply_records_at_revision_reject(t, namespaces, active_writers); return; }
  if (t->abort_status == lb_abort_restart) { lb_apply_records_at_revision_restart(t, namespaces, epoch, active_writers); return; }
  t->waiting_capacity = false;
  if (t->phase == lb_task_new) {
    uint64_t version = lb_namespace_revision(namespaces, t->ns);
    bool absent = !lb_directory_entry(namespaces, t->ns);
    if (!w->writable) { lb_apply_records_at_revision_reject(t, namespaces, active_writers); return; }
    if (lb_namespace_writing(namespaces, t->ns)) {
      lb_reply_header(&t->reply, lb_status_retry, epoch, version);
      t->phase = lb_task_done;
      return;
    }
    if (t->epoch != epoch || t->version != version) { lb_apply_records_at_revision_restart(t, namespaces, epoch, active_writers); return; }
    if (!lb_decode_record_batch(&w->batch, lb_buffer_bytes(&t->payload))) { lb_apply_records_at_revision_reject(t, namespaces, active_writers); return; }
    /* This operation owns namespace creation, the write guard and its revision. */
    size_t slots = CT_MIN(w->batch.count, CT_MIN(w->write_limit, records->pending_limit));
    if (!lb_pending_reserve(&t->pending, slots)) { lb_apply_records_at_revision_reject(t, namespaces, active_writers); return; }
    w->before = t->version;
    if (absent && lb_store_create_namespace(records, t->ns).code != lb_error_ok) {
      lb_apply_records_at_revision_reject(t, namespaces, active_writers); return;
    }
    lb_namespace_entry *entry = lb_directory_entry(namespaces, t->ns);
    entry->writing = true;
    w->locked = true;
    ++*active_writers;
    if (absent || w->batch.count) entry->revision = ++t->version;
    t->phase = lb_task_active;
  }
  lb_batch_result result = lb_write_record_batch(&w->batch, &t->pending, records, t->ns,
                                                *active_writers, budget);
  switch (result) {
  case lb_batch_pending: return;
  case lb_batch_capacity: t->waiting_capacity = true; return;
  case lb_batch_conflict: lb_apply_records_at_revision_restart(t, namespaces, epoch, active_writers); return;
  case lb_batch_failed: lb_apply_records_at_revision_reject(t, namespaces, active_writers); return;
  case lb_batch_finished: break;
  }
  lb_reply_header(&t->reply, lb_status_ok, epoch, t->version);
  if (t->reply.failed) { lb_apply_records_at_revision_reject(t, namespaces, active_writers); return; }
  t->publication = (lb_publication) {w->before, t->version, epoch,
                               lb_namespace_fingerprint(namespaces, t->ns), w->batch.records_payload};
  w->succeeded = true;
  t->phase = lb_task_done;
  t->publish = w->before != t->version;
  lb_apply_records_at_revision_release(t, namespaces, active_writers);
}
static inline void lb_apply_records_at_revision_resume(lb_task *t, lb_directory *namespaces, uint64_t epoch,
                                      lb_store *records, size_t *active_writers, size_t budget) {
  lb_apply_records_at_revision_step(t, namespaces, epoch, records, active_writers, budget);
  lb_apply_records_at_revision_state *w = (lb_apply_records_at_revision_state *)t->state;
  if (w) t->state_bytes = sizeof(*w) + (w->batch.changes ? w->batch.count * sizeof(lb_change) : 0);
}
static inline bool lb_apply_records_at_revision_begin(lb_task *t, channel *ch, const lb_request *request, size_t write_limit) {
  if (!ch || !lb_auth_verify(&ch->auth, request)) return false;
  bool ok = lb_apply_records_at_revision_prepare(t, lb_name(ch->name), true, request->epoch,
                                               request->revision, request->payload, write_limit);
  return ok;
}
static inline bool lb_apply_records_at_revision_finish(lb_task *t, channel *ch, lb_subscribers *subscribers) {
  if (!lb_reply_valid(&t->reply)) return false;
  uint8_t status = t->reply.data[0] & 0x7f;
  if (status == lb_status_absent || (status == lb_status_retry && t->reply.size > 17))
    subscription_reset_revision(subscribers, ch,
                                lb_integer(lb_bytes_make(t->reply.data + 9, 8)));
  return true;
}
#endif
