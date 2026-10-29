#ifndef LB_OP_2_FETCH_RECORDS_H
#define LB_OP_2_FETCH_RECORDS_H
#include "models.h"
#include "namespaces.h"
#include "exchange.h"
#include "index_sync.h"
#include "storage.h"
#include "request_lifetime.h"
#include "subscriber_delivery.h"
/* Operation 2: fetch a stable revision. Recheck after every I/O yield so a
 * concurrent owner cannot produce a reply mixing different revisions. */
typedef struct lb_fetch_records_state {
  uint64_t *keys;
  size_t count, position;
  lb_buffer records, read_buffer;
} lb_fetch_records_state;
static inline bool lb_fetch_records_prepare(lb_task *t, lb_namespace ns, uint64_t epoch,
                                           uint64_t revision, lb_bytes payload) {
  return lb_task_prepare(t, lb_op_fetch_records, ns, epoch, revision, payload) &&
      lb_task_allocate(t, sizeof(lb_fetch_records_state));
}
static inline void lb_fetch_records_dispose(lb_task *t) {
  lb_fetch_records_state *f = (lb_fetch_records_state *)t->state;
  if (!lb_task_drain(t)) abort();
  if (f) {
    free(f->keys);
    lb_buffer_free(&f->records);
    lb_buffer_free(&f->read_buffer);
  }
  lb_task_dispose(t);
}
static inline void lb_fetch_records_reject(lb_task *t) {
  if (!lb_task_stop(t, lb_abort_reject)) return;
  lb_reply_rejected(&t->reply);
  t->phase = lb_task_done;
}
static inline int lb_fetch_key_order(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}
static inline bool lb_fetch_records_decode(lb_fetch_records_state *f, lb_bytes payload) {
  lb_reader r = {payload, false};
  uint32_t count = lb_read_u32(&r);
  if (r.failed || count > LB_MAX_RECORDS || count > r.data.size / 8) return false;
  if (count) {
    f->keys = (uint64_t *)calloc(count, sizeof(*f->keys));
    if (!f->keys) return false;
  }
  f->count = count;
  for (size_t i = 0; i < count; ++i) f->keys[i] = lb_read_u64(&r);
  if (!lb_reader_end(&r)) return false;
  if (count > 1) qsort(f->keys, count, sizeof(*f->keys), lb_fetch_key_order);
  for (size_t i = 1; i < count; ++i)
    if (f->keys[i - 1] == f->keys[i]) return false;
  return true;
}
static inline bool lb_fetch_records_restart_reply(lb_task *t, const lb_directory *namespaces, uint64_t epoch) {
  lb_namespace_entry *entry = lb_directory_entry(namespaces, t->ns);
  bool busy = entry && entry->writing;
  lb_reply_header(&t->reply, entry ? lb_status_retry : lb_status_absent, epoch,
                  entry ? entry->revision : 0);
  if (entry && !busy && !lb_exchange_begin(lb_index_source(&entry->value), &t->reply)) return false;
  return !t->reply.failed;
}
static inline void lb_fetch_records_restart(lb_task *t, const lb_directory *namespaces, uint64_t epoch) {
  if (!lb_task_stop(t, lb_abort_restart)) return;
  if (!lb_fetch_records_restart_reply(t, namespaces, epoch)) { lb_fetch_records_reject(t); return; }
  t->phase = lb_task_done;
}
static inline void lb_fetch_records_step(lb_task *t, lb_directory *namespaces, uint64_t epoch, lb_store *records, size_t budget) {
  lb_fetch_records_state *f = (lb_fetch_records_state *)t->state;
  if (t->phase == lb_task_done) return;
  if (t->abort_status == lb_abort_reject) { lb_fetch_records_reject(t); return; }
  if (t->abort_status == lb_abort_restart) { lb_fetch_records_restart(t, namespaces, epoch); return; }
  t->waiting_capacity = false;
  uint64_t version = lb_namespace_revision(namespaces, t->ns);
  bool busy = lb_namespace_writing(namespaces, t->ns);
  if (t->phase == lb_task_new && busy) {
    lb_reply_header(&t->reply, lb_status_retry, epoch, version);
    t->phase = lb_task_done;
    return;
  }
  if (t->phase != lb_task_new && t->pending.first.state == lb_phase_finished && t->pending.first.outcome.code != lb_error_ok) {
    lb_fetch_records_reject(t); return;
  }
  if (t->epoch != epoch || t->version != version || busy ||
      !lb_directory_entry(namespaces, t->ns)) {
    lb_fetch_records_restart(t, namespaces, epoch); return;
  }
  if (t->phase == lb_task_new) {
    if (!lb_fetch_records_decode(f, lb_buffer_bytes(&t->payload))) { lb_fetch_records_reject(t); return; }
    lb_u32(&f->records, (uint32_t)f->count);
    t->phase = lb_task_active;
  } else if (t->pending.first.state == lb_phase_finished) {
    if (t->pending.first.outcome.code != lb_error_ok) { lb_fetch_records_reject(t); return; }
    lb_u64(&f->records, f->keys[f->position]);
    lb_blob(&f->records, lb_bytes_make(f->read_buffer.data, t->pending.first.transferred));
    if (f->records.failed || f->records.size > LB_MAX_FRAME - 32) {
      lb_fetch_records_reject(t); return;
    }
    ++f->position;
    t->pending.first.state = lb_phase_idle;
  } else if (t->pending.first.owner) return;
  while (f->position < f->count) {
    lb_store_result rc;
    if (!budget--) return;
    if (!lb_buffer_reserve(&f->read_buffer, 65535)) { lb_fetch_records_reject(t); return; }
    rc = lb_store_prepare_read(records, &t->pending.first, t->ns,
                              f->keys[f->position], f->read_buffer.data, 65535);
    if (rc.code == lb_error_pending_full) { t->waiting_capacity = true; return; }
    if (rc.code == lb_error_busy) { lb_fetch_records_restart(t, namespaces, epoch); return; }
    if (rc.code != lb_error_ok) { lb_fetch_records_reject(t); return; }
    if (t->pending.first.owner) return;
    ++f->position;
  }
  lb_reply_header(&t->reply, lb_status_ok, epoch, t->version);
  if (!lb_append(&t->reply, f->records.data, f->records.size)) { lb_fetch_records_reject(t); return; }
  t->phase = lb_task_done;
}
static inline void lb_fetch_records_resume(lb_task *t, lb_directory *namespaces, uint64_t epoch,
                                          lb_store *records, size_t budget) {
  lb_fetch_records_step(t, namespaces, epoch, records, budget);
  lb_fetch_records_state *f = (lb_fetch_records_state *)t->state;
  if (f) t->state_bytes = sizeof(*f) + (f->keys ? f->count * sizeof(*f->keys) : 0) +
      f->records.capacity + f->read_buffer.capacity;
}
static inline bool lb_fetch_records_begin(lb_task *t, channel *ch, const lb_request *request) {
  if (!ch) return false;
  bool ok = lb_fetch_records_prepare(t, lb_name(ch->name),
                            request->epoch, request->revision, request->payload);
  return ok;
}
static inline bool lb_fetch_records_finish(lb_task *t, channel *ch, lb_subscribers *subscribers) {
  if (!lb_reply_valid(&t->reply)) return false;
  uint8_t status = t->reply.data[0] & 0x7f;
  if (status == lb_status_absent || (status == lb_status_retry && t->reply.size > 17))
    subscription_reset_revision(subscribers, ch,
                                lb_integer(lb_bytes_make(t->reply.data + 9, 8)));
  return true;
}
#endif
