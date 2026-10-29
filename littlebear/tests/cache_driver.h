#ifndef LB_TEST_CACHE_DRIVER_H
#define LB_TEST_CACHE_DRIVER_H
#include "../op_0_begin_owner_sync.h"
#include "../op_1_continue_reconciliation.h"
#include "../op_2_fetch_records.h"
#include "../op_3_apply_records_at_revision.h"
#include "../op_6_stream_record_changes.h"
/* Test-only in-process driver for exercising the real operation state machines.
 * Tests supply an already-authorized writable flag; network authorization remains
 * in the operation begin handlers reached through server.c. */
typedef struct lb_test_cache {
  lb_store cache;
  uint64_t epoch, generation;
  size_t active_writers;
  /* One notification after unlock. A verified update carries borrowed records
   * valid for this call; abort/restart or an unchanged revision passes NULL. */
  void (*changed)(void *context, lb_namespace ns, const lb_publication *publication);
  void *changed_context;
} lb_test_cache;
static inline lb_namespace_entry *lb_test_cache_entry(lb_test_cache *s, lb_namespace ns) {
  return lb_directory_entry(&s->cache.namespaces, ns);
}
static inline uint64_t lb_test_cache_revision(lb_test_cache *s, lb_namespace ns) {
  lb_namespace_entry *e = lb_test_cache_entry(s, ns);
  return e ? e->revision : 0;
}
static inline bool lb_test_cache_writing(lb_test_cache *s, lb_namespace ns) {
  lb_namespace_entry *e = lb_test_cache_entry(s, ns);
  return e && e->writing;
}
static inline uint64_t lb_test_cache_fingerprint(lb_test_cache *s, lb_namespace ns) {
  lb_namespace_entry *e = lb_test_cache_entry(s, ns);
  return e ? ct_node_hash(e->value.root) : 0;
}
static inline lb_store_result lb_test_cache_create(lb_test_cache *s, const char *path, uint64_t epoch,
                                                uint64_t capacity) {
  lb_store_result rc = lb_store_create(&s->cache, path, capacity, 64, 0, NULL, ct_system_allocate);
  if (rc.code == lb_error_ok)
    s->epoch = epoch;
  return rc;
}
static inline void lb_test_cache_destroy(lb_test_cache *s) {
  lb_store_result rc = lb_store_destroy(&s->cache);
  assert(rc.code == lb_error_ok);
  (void)rc;
}
static inline void lb_test_flush(lb_task *t, lb_test_cache *s) {
  if (!t->changed) return;
  ++s->generation;
  if (s->changed) s->changed(s->changed_context, t->ns, t->publish ? &t->publication : NULL);
  t->changed = false;
}
static inline void lb_test_task_dispose(lb_task *t, lb_test_cache *s) {
  if (!lb_task_drain(t)) abort();
  switch (t->operation) {
  case 3: lb_apply_records_at_revision_release(t, &s->cache.namespaces, &s->active_writers); break;
  case 6: lb_stream_record_changes_release(t, &s->cache.namespaces, &s->active_writers); break;
  default: break;
  }
  lb_test_flush(t, s);
  switch (t->operation) {
  case 2: lb_fetch_records_dispose(t); break;
  case 3: lb_apply_records_at_revision_dispose(t); break;
  case 6: lb_stream_record_changes_dispose(t); break;
  default: lb_task_dispose(t); break;
  }
}
static inline void lb_test_task_step_budget(lb_task *t, lb_test_cache *s, size_t budget) {
  switch (t->operation) {
  case 0: lb_begin_owner_sync_resume(t, &s->cache.namespaces, s->epoch, budget); break;
  case 1: lb_continue_reconciliation_resume(t, &s->cache.namespaces, s->epoch, budget); break;
  case 2: lb_fetch_records_resume(t, &s->cache.namespaces, s->epoch, &s->cache, budget); break;
  case 3: lb_apply_records_at_revision_resume(t, &s->cache.namespaces, s->epoch, &s->cache, &s->active_writers, budget); break;
  case 6: lb_stream_record_changes_resume(t, &s->cache.namespaces, s->epoch, &s->cache, &s->active_writers, budget); break;
  default: lb_reply_rejected(&t->reply); t->phase = lb_task_done; break;
  }
  lb_test_flush(t, s);
}
static inline void lb_test_task_step(lb_task *t, lb_test_cache *s) {
  lb_test_task_step_budget(t, s, SIZE_MAX);
}
static inline void lb_test_task_restart(lb_task *t, lb_test_cache *s) {
  t->abort_status = lb_abort_restart;
  lb_test_task_step(t, s);
}
static inline bool lb_test_task_prepare(lb_task *t, unsigned op, lb_namespace ns, bool writable,
                                        uint64_t epoch, uint64_t revision, lb_bytes payload) {
  bool ok;
  switch (op) {
  case 2: ok = lb_fetch_records_prepare(t, ns, epoch, revision, payload); break;
  case 3: ok = lb_apply_records_at_revision_prepare(t, ns, writable, epoch, revision, payload, 1); break;
  case 6: ok = lb_stream_record_changes_prepare(t, ns, writable, epoch, revision, payload, 1); break;
  default: ok = lb_task_prepare(t, op, ns, epoch, revision, payload); break;
  }
  if (!ok) { lb_reply_rejected(&t->reply); t->phase = lb_task_done; }
  return ok;
}
/* These test queries cross the operation boundary deliberately; production
 * transport accounting and scheduling never inspect operation-private state. */
static inline lb_record_batch *lb_test_write_batch(lb_task *t) {
  assert(t->state && (t->operation == 3 || t->operation == 6));
  return t->operation == 3 ? &((lb_apply_records_at_revision_state *)t->state)->batch :
                            &((lb_stream_record_changes_state *)t->state)->batch;
}
static inline bool lb_test_write_locked(lb_task *t) {
  return t->operation == 3 ? ((lb_apply_records_at_revision_state *)t->state)->locked :
                            ((lb_stream_record_changes_state *)t->state)->locked;
}
static inline bool lb_test_write_succeeded(lb_task *t) {
  return t->operation == 3 ? ((lb_apply_records_at_revision_state *)t->state)->succeeded :
                            ((lb_stream_record_changes_state *)t->state)->succeeded;
}
static inline void lb_test_write_window(lb_task *t, size_t slots) {
  if (t->operation == 3) ((lb_apply_records_at_revision_state *)t->state)->write_limit = slots;
  else ((lb_stream_record_changes_state *)t->state)->write_limit = slots;
}
static inline lb_stream_record_changes_state *lb_test_stream(lb_task *t) {
  assert(t->operation == 6);
  return (lb_stream_record_changes_state *)t->state;
}
static inline lb_store_result lb_test_perform(lb_store_request *request) {
  for (;;) {
    lb_store_io_operation op = CT_ZERO;
    lb_store_result rc = lb_store_start_io(request, &op);
    ssize_t n;
    if (rc.code != lb_error_ok)
      return rc;
    n = op.writing ? pwrite(op.fd, op.buffer, op.length, (off_t)op.offset)
                   : pread(op.fd, op.buffer, op.length, (off_t)op.offset);
    rc = lb_store_complete_io(request, n < 0 ? -errno : (int)n);
    if (rc.code != lb_error_more_io)
      return rc;
  }
}
static inline bool lb_test_handle(lb_test_cache *s, unsigned op, lb_namespace ns, bool writable,
                             uint64_t epoch, uint64_t revision, lb_bytes payload,
                             lb_buffer *reply) {
  lb_task t;
  lb_test_task_prepare(&t, op, ns, writable, epoch, revision, payload);
  while (t.phase != lb_task_done) {
    lb_test_task_step(&t, s);
    /* This synchronous convenience API cannot drive another task's I/O. Keep
     * its existing retry response instead of spinning on external work. */
    if (t.waiting_capacity)
      lb_test_task_restart(&t, s);
    if (t.pending.first.owner)
      lb_test_perform(&t.pending.first);
  }
  *reply = t.reply;
  memset(&t.reply, 0, sizeof(t.reply));
  lb_test_task_dispose(&t, s);
  return !reply->failed;
}
#endif
