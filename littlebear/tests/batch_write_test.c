#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "cache_driver.h"
#include <stdio.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
static size_t publications, notifications;
static void changed(void *context, lb_namespace ns, const lb_publication *publication) {
  lb_test_cache *s = context;
  CHECK(!lb_test_cache_writing(s, ns));
  CHECK(++notifications == s->generation);
  if (!publication) return;
  CHECK(publication->after == publication->before + 1 && publication->epoch == s->epoch);
  CHECK(!s->cache.pending_count);
  CHECK(publication->fingerprint == lb_test_cache_fingerprint(s, ns) && publication->records.size > 4);
  ++publications;
}
static lb_buffer payload(size_t count, bool mixed) {
  lb_buffer b = {0};
  uint64_t fingerprint = 0;
  lb_u64(&b, 0); lb_u32(&b, (uint32_t)count);
  for (size_t i = 1; i <= count; ++i) {
    lb_buffer key = {0}, value = {0};
    bool present = !mixed || i % 3;
    lb_u64(&b, i); lb_u8(&b, present);
    if (!present) continue;
    lb_u64(&key, i); lb_u64(&key, i * 9);
    uint64_t hash = XXH64(key.data, key.size, 0);
    fingerprint ^= hash;
    lb_u64(&value, hash); lb_u64(&value, i * 9);
    lb_blob(&b, lb_buffer_bytes(&value));
    lb_buffer_free(&key); lb_buffer_free(&value);
  }
  for (unsigned i = 0; i < 8; ++i) b.data[i] = (uint8_t)(fingerprint >> (56 - i * 8));
  return b;
}
static void create(lb_test_cache *s) {
  char path[] = "/tmp/littlebear-batch-XXXXXX";
  int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
  CHECK(lb_test_cache_create(s, path, 19, 1 << 20).code == lb_error_ok);
  unlink(path);
  s->changed = changed; s->changed_context = s;
  publications = notifications = 0;
}
static void begin(lb_task *t, lb_test_cache *s, size_t count, size_t window, bool mixed) {
  lb_buffer b = payload(count, mixed);
  lb_namespace ns = {9, 2};
  CHECK(lb_test_task_prepare(t, 6, ns, true, s->epoch, lb_test_cache_revision(s, ns), lb_buffer_bytes(&b)));
  lb_test_write_window(t, window);
  lb_buffer_free(&b);
  lb_test_task_step(t, s);
}
static lb_store_io_operation submit(lb_store_request *r) {
  lb_store_io_operation op = {0};
  CHECK(lb_store_start_io(r, &op).code == lb_error_ok);
  return op;
}
static void finish_io(lb_store_request *r, lb_store_io_operation op) {
  ssize_t n = pwrite(op.fd, op.buffer, op.length, (off_t)op.offset);
  CHECK(n == op.length);
  CHECK(lb_store_complete_io(r, (int)n).code == lb_error_ok);
}
static void drive(lb_task *t, lb_test_cache *s) {
  size_t turns = 0;
  while (t->phase != lb_task_done) {
    CHECK(++turns < 1000 && !publications);
    /* Complete highest slots first; lower slots remain live across refills. */
    lb_store_request *chosen = NULL;
    lb_store_io_operation operation = {0};
    for (size_t i = 0; i <= t->pending.extra_count; ++i) {
      lb_store_request *r = lb_task_io(t, i);
      if (r->owner && r->state == lb_phase_ready) {
        lb_store_io_operation op = submit(r);
        chosen = r; operation = op;
      } else if (r->owner && r->state == lb_phase_submitted) {
        chosen = r;
        operation = (lb_store_io_operation){.fd = r->owner->values.file_descriptor,
            .offset = lb_voffset(r->value) + r->transferred,
            .buffer = (uint8_t *)r->buffer + r->transferred,
            .length = (uint32_t)(lb_vsize(r->value) - r->transferred), .writing = true};
      }
    }
    CHECK(chosen);
    finish_io(chosen, operation);
    lb_test_task_step(t, s);
  }
  CHECK(publications == 1 && !lb_test_write_locked(t) && lb_test_write_succeeded(t));
  if (lb_test_stream(t)->no_ack) CHECK(!t->reply.data && !t->reply.size && !t->reply.capacity);
  else CHECK(t->reply.size == 17 && t->reply.data[0] == 0);
}
static void overlap_and_partial(void) {
  lb_test_cache s = {0}; lb_task t = {0}; create(&s);
  begin(&t, &s, 40, 4, false);
  CHECK(t.pending.extra_count == 3 && lb_test_write_batch(&t)->position == 4 && s.cache.pending_count == 4);
  CHECK(lb_test_write_locked(&t) && !publications);
  lb_store_io_operation op = submit(&t.pending.first);
  CHECK(lb_store_complete_io(&t.pending.first, -EAGAIN).code == lb_error_more_io);
  CHECK(t.pending.first.state == lb_phase_ready && s.cache.pending_count == 4);
  op = submit(&t.pending.first);
  CHECK(pwrite(op.fd, op.buffer, 5, (off_t)op.offset) == 5);
  CHECK(lb_store_complete_io(&t.pending.first, 5).code == lb_error_more_io);
  CHECK(t.pending.first.transferred == 5 && !publications);
  /* Grow/repack namespace metadata while all request pointers stay pinned. */
  for (uint64_t i = 100; i < 500; ++i)
    CHECK(lb_store_create_namespace(&s.cache, (lb_namespace){i, 3}).code == lb_error_ok);
  drive(&t, &s);
  CHECK(lb_index_summary(lb_directory_get(&s.cache.namespaces, t.ns),
                        (ct_range){0, 0, false}).count == 40);
  lb_test_task_dispose(&t, &s); lb_test_cache_destroy(&s);
}
static void failure_drains(void) {
  lb_test_cache s = {0}; lb_task t = {0}; create(&s);
  begin(&t, &s, 12, 4, false);
  lb_store_io_operation a = submit(&t.pending.first), b = submit(&t.pending.extra[0]);
  (void)a;
  CHECK(lb_store_complete_io(&t.pending.first, -EIO).code == lb_error_io_failure);
  lb_test_task_step(&t, &s);
  CHECK(t.abort_status == 2 && lb_test_write_locked(&t) && t.phase != lb_task_done);
  CHECK(lb_test_write_batch(&t)->position == 4 && s.cache.pending_count == 1 && !publications);
  CHECK(!t.pending.extra[1].owner && !t.pending.extra[2].owner);
  finish_io(&t.pending.extra[0], b);
  lb_test_task_step(&t, &s);
  CHECK(t.phase == lb_task_done && t.reply.data[0] == 2 && !lb_test_write_locked(&t));
  CHECK(!publications && notifications == 1 && !s.cache.pending_count);
  lb_test_task_dispose(&t, &s); lb_test_cache_destroy(&s);
}
static void restart_drains_before_reply(void) {
  for (unsigned operation = 3; operation <= 6; operation += 3) {
    lb_test_cache s = {0}; lb_task t = {0}; create(&s);
    lb_buffer b = payload(12, false);
    size_t prefix = operation == 3 ? 8 : 0;
    CHECK(lb_test_task_prepare(&t, operation, (lb_namespace){9, 2}, true, s.epoch, 0,
                            lb_bytes_make(b.data + prefix, b.size - prefix)));
    lb_buffer_free(&b);
    lb_test_write_window(&t, 4);
    lb_test_task_step(&t, &s);
    lb_store_io_operation a = submit(&t.pending.first), pending = submit(&t.pending.extra[0]);
    lb_test_task_restart(&t, &s);
    CHECK(t.abort_status == 1 && lb_test_write_locked(&t) && t.phase != lb_task_done);
    CHECK(s.cache.pending_count == 2 && !t.reply.size && !notifications);
    CHECK(!t.pending.extra[1].owner && !t.pending.extra[2].owner);
    finish_io(&t.pending.first, a);
    lb_test_task_step(&t, &s);
    CHECK(lb_test_write_locked(&t) && !t.reply.size && !notifications && s.cache.pending_count == 1);
    finish_io(&t.pending.extra[0], pending);
    lb_test_task_step(&t, &s);
    /* The operation builds a busy reply while its write lock is still held;
     * the lifecycle then releases the lock and wakes subscribers exactly once. */
    CHECK(t.phase == lb_task_done && t.reply.size == 17 && t.reply.data[0] == 1);
    CHECK(lb_integer(lb_bytes_make(t.reply.data + 1, 8)) == s.epoch);
    CHECK(!lb_test_write_locked(&t) && !lb_test_write_succeeded(&t) && !s.cache.pending_count && !publications);
    CHECK(notifications == 1);
    lb_test_task_dispose(&t, &s);
    CHECK(notifications == 1);
    lb_test_cache_destroy(&s);
  }
}
static void capacity_and_cancel(void) {
  lb_test_cache s = {0}; lb_task t = {0}; lb_store_request blocker = {0}; create(&s);
  s.cache.pending_limit = 3;
  lb_namespace other = {77, 1};
  CHECK(lb_store_create_namespace(&s.cache, other).code == lb_error_ok);
  CHECK(lb_store_prepare_write(&s.cache, &blocker, other, 1, "data", 4, 0).code == lb_error_ok);
  begin(&t, &s, 12, 4, false);
  CHECK(t.pending.extra_count == 2 && lb_test_write_batch(&t)->position == 2 && t.waiting_capacity);
  CHECK(s.cache.pending_count == 3 && lb_task_io_ready(&t));
  CHECK(lb_store_cancel(&blocker).code == lb_error_cancelled);
  lb_test_task_step(&t, &s);
  CHECK(!t.waiting_capacity && lb_test_write_batch(&t)->position == 3 && s.cache.pending_count == 3);
  drive(&t, &s);
  lb_test_task_dispose(&t, &s);
  publications = 0;
  begin(&t, &s, 12, 4, false);
  CHECK(s.cache.pending_count == 3);
  lb_test_task_dispose(&t, &s);
  CHECK(!s.cache.pending_count && !publications && !lb_test_cache_writing(&s, (lb_namespace){9, 2}));
  lb_test_cache_destroy(&s);
}
static void mixed_changes(void) {
  lb_test_cache s = {0}; lb_task t = {0}; create(&s);
  begin(&t, &s, 24, 4, false); drive(&t, &s); lb_test_task_dispose(&t, &s);
  publications = 0;
  begin(&t, &s, 24, 4, true); drive(&t, &s);
  CHECK(lb_index_summary(lb_directory_get(&s.cache.namespaces, t.ns),
                        (ct_range){0, 0, false}).count == 16);
  lb_test_task_dispose(&t, &s); lb_test_cache_destroy(&s);
}
static void mismatch_and_retry_drain(void) {
  lb_test_cache s = {0}; lb_task t = {0}; create(&s);
  begin(&t, &s, 4, 4, false);
  lb_test_stream(&t)->expected ^= 1;
  for (size_t i = 0; i <= t.pending.extra_count; ++i) {
    lb_store_request *r = lb_task_io(&t, i);
    finish_io(r, submit(r));
    CHECK(!publications && lb_test_write_locked(&t));
  }
  lb_test_task_step(&t, &s);
  CHECK(t.phase == lb_task_done && t.reply.data[0] == 1 && !publications && !lb_test_write_locked(&t));
  lb_test_task_dispose(&t, &s);
  begin(&t, &s, 4, 4, false);
  submit(&t.pending.first); submit(&t.pending.extra[0]);
  CHECK(lb_store_complete_io(&t.pending.first, -EIO).code == lb_error_io_failure);
  lb_test_task_step(&t, &s);
  CHECK(lb_test_write_locked(&t) && t.abort_status == 2 && s.cache.pending_count == 1);
  /* A retryable completion after another write failed must be cancelled,
   * not resubmitted, and must release the final reservation and lock. */
  CHECK(lb_store_complete_io(&t.pending.extra[0], -EAGAIN).code == lb_error_more_io);
  lb_test_task_step(&t, &s);
  CHECK(t.phase == lb_task_done && t.reply.data[0] == 2 && !lb_test_write_locked(&t));
  CHECK(!publications && !s.cache.pending_count);
  lb_test_task_dispose(&t, &s); lb_test_cache_destroy(&s);
}
static void fair_share(void) {
  lb_test_cache s = {0}; lb_task t = {0}, others[3] = {0}; create(&s);
  s.cache.pending_limit = 8;
  begin(&t, &s, 24, 4, false);
  lb_buffer b = payload(24, false);
  for (size_t i = 0; i < 3; ++i) {
    CHECK(lb_test_task_prepare(&others[i], 6, (lb_namespace){20 + i, 2}, true,
                       s.epoch, 0, lb_buffer_bytes(&b)));
    lb_test_write_window(&others[i], 4);
    lb_test_task_step(&others[i], &s);
  }
  lb_buffer_free(&b);
  CHECK(s.active_writers == 4 && s.cache.pending_count == 8);
  for (size_t i = 0; i < 4; ++i) {
    lb_store_io_operation op = submit(lb_task_io(&t, i));
    if (i < 3) finish_io(lb_task_io(&t, i), op);
  }
  lb_test_task_step(&t, &s);
  /* One old operation remains; the shrunken share admits only one more. */
  CHECK(lb_test_write_batch(&t)->position == 5 && s.cache.pending_count == 6 && !publications);
  for (size_t i = 0; i < 3; ++i) lb_test_task_dispose(&others[i], &s);
  CHECK(s.active_writers == 1);
  lb_test_task_step(&t, &s);
  CHECK(lb_test_write_batch(&t)->position == 7 && s.cache.pending_count == 4);
  drive(&t, &s);
  CHECK(!s.active_writers);
  lb_test_task_dispose(&t, &s); lb_test_cache_destroy(&s);
}
static void no_ack_completion(void) {
  lb_test_cache s = {0}; lb_task t = {0}; create(&s);
  begin(&t, &s, 4, 4, false);
  lb_test_stream(&t)->no_ack = true;
  drive(&t, &s);
  CHECK(notifications == 1);
  lb_test_task_dispose(&t, &s);
  CHECK(notifications == 1); /* Disposal cannot notify twice. */
  publications = 0;
  begin(&t, &s, 4, 4, false);
  lb_test_stream(&t)->no_ack = true;
  lb_test_stream(&t)->expected ^= 1;
  for (size_t i = 0; i <= t.pending.extra_count; ++i) {
    lb_store_request *r = lb_task_io(&t, i);
    finish_io(r, submit(r));
  }
  lb_test_task_step(&t, &s);
  CHECK(t.phase == lb_task_done && !lb_test_write_succeeded(&t) && t.reply.data[0] == 1);
  CHECK(notifications == 2 && !publications);
  lb_test_task_dispose(&t, &s);
  begin(&t, &s, 4, 4, false);
  lb_test_stream(&t)->no_ack = true;
  submit(&t.pending.first);
  CHECK(lb_store_complete_io(&t.pending.first, -EIO).code == lb_error_io_failure);
  lb_test_task_step(&t, &s);
  CHECK(t.phase == lb_task_done && !lb_test_write_succeeded(&t) && t.reply.data[0] == 2);
  CHECK(notifications == 3 && !publications);
  lb_test_task_dispose(&t, &s); lb_test_cache_destroy(&s);
}
/* Operations return effects without reaching into the caller's subscriber state.
 * Borrowed publication bytes remain alive until the caller consumes the effect. */
static void explicit_publication_lifetime(void) {
  lb_test_cache s = {0}; lb_task t = {0}; create(&s);
  lb_buffer b = payload(1, false);
  lb_namespace ns = {9, 2};
  CHECK(lb_test_task_prepare(&t, 6, ns, true, s.epoch, 0, lb_buffer_bytes(&b)));
  lb_buffer_free(&b);
  lb_stream_record_changes_resume(&t, &s.cache.namespaces, s.epoch, &s.cache, &s.active_writers, 64);
  CHECK(lb_test_write_locked(&t) && s.active_writers == 1 && !t.changed && !notifications);
  CHECK(lb_test_perform(&t.pending.first).code == lb_error_ok);
  lb_stream_record_changes_resume(&t, &s.cache.namespaces, s.epoch, &s.cache, &s.active_writers, 64);
  CHECK(t.phase == lb_task_done && !lb_test_write_locked(&t) && !s.active_writers);
  CHECK(t.changed && t.publish && !notifications && !publications);
  CHECK(t.publication.records.data == lb_test_write_batch(&t)->records_payload.data);
  CHECK(t.publication.records.size == lb_test_write_batch(&t)->records_payload.size && t.publication.records.size > 4);
  CHECK(t.publication.before == 0 && t.publication.after == 1);
  lb_test_flush(&t, &s);
  lb_test_flush(&t, &s);
  CHECK(notifications == 1 && publications == 1 && !t.changed);
  lb_stream_record_changes_dispose(&t);
  lb_test_cache_destroy(&s);
}
static void write_operation_boundaries(void) {
  const unsigned operations[] = {3, 6};
  for (size_t i = 0; i < 2; ++i) {
    unsigned operation = operations[i];
    lb_test_cache s = {0}; lb_task t = {0}; create(&s);
    s.changed = NULL; /* Inspect effects directly, including empty publications. */
    lb_namespace ns = {9, 2};
    lb_buffer invalid = {0};
    if (operation == 6) lb_u64(&invalid, 0);
    lb_u32(&invalid, 1); /* Missing the claimed record. */
    CHECK(lb_test_task_prepare(&t, operation, ns, true, s.epoch, 0, lb_buffer_bytes(&invalid)));
    lb_test_task_step(&t, &s);
    CHECK(t.phase == lb_task_done && t.reply.data[0] == 2 && !s.active_writers);
    CHECK(!lb_test_cache_entry(&s, ns) && !s.generation);
    lb_test_task_dispose(&t, &s); lb_buffer_free(&invalid);

    lb_buffer empty = payload(0, false);
    size_t prefix = operation == 3 ? 8 : 0;
    lb_bytes bytes = lb_bytes_make(empty.data + prefix, empty.size - prefix);
    CHECK(lb_test_task_prepare(&t, operation, ns, false, s.epoch, 0, bytes));
    lb_test_task_step(&t, &s);
    CHECK(t.reply.data[0] == 2 && !lb_test_cache_entry(&s, ns) && !s.generation);
    lb_test_task_dispose(&t, &s);

    CHECK(lb_test_task_prepare(&t, operation, ns, true, s.epoch, 0, bytes));
    lb_test_task_step(&t, &s);
    CHECK(t.reply.data[0] == 0 && t.publish && s.generation == 1);
    CHECK(lb_test_cache_revision(&s, ns) == 1 && !s.active_writers);
    lb_test_task_dispose(&t, &s);
    CHECK(lb_test_task_prepare(&t, operation, ns, true, s.epoch, 1, bytes));
    lb_test_task_step(&t, &s);
    CHECK(t.reply.data[0] == 0 && !t.publish && s.generation == 2);
    CHECK(lb_test_cache_revision(&s, ns) == 1 && !s.active_writers);
    lb_test_task_dispose(&t, &s); lb_buffer_free(&empty);

    lb_buffer change = payload(1, false);
    CHECK(lb_test_task_prepare(&t, operation, ns, true, s.epoch, 1,
                              lb_bytes_make(change.data + prefix, change.size - prefix)));
    lb_test_task_step_budget(&t, &s, 0);
    CHECK(t.phase == lb_task_active && lb_test_write_locked(&t) && s.active_writers == 1);
    CHECK(lb_test_cache_revision(&s, ns) == 2 && !s.cache.pending_count);
    CHECK(lb_test_write_batch(&t)->position == 0 && s.generation == 2);
    lb_test_task_dispose(&t, &s);
    CHECK(!lb_test_cache_writing(&s, ns) && !s.active_writers && s.generation == 3);
    lb_buffer_free(&change); lb_test_cache_destroy(&s);
  }
}
int main(void) {
  overlap_and_partial(); failure_drains(); capacity_and_cancel(); mixed_changes();
  mismatch_and_retry_drain();
  restart_drains_before_reply();
  fair_share();
  no_ack_completion();
  explicit_publication_lifetime();
  write_operation_boundaries();
  puts("PASS: concurrent batch writes, out-of-order refill, partial I/O, capacity, failure drain, cancellation, mixed changes and explicit publication lifetime");
  return 0;
}
