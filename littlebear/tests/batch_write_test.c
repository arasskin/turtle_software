#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "../sync.h"
#include <stdio.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
static size_t publications, notifications;
static void changed(void *context, lb_namespace ns, const lb_publication *publication) {
  lb_service *s = context;
  CHECK(!lb_service_writing(s, ns));
  CHECK(++notifications == s->generation);
  if (!publication) return;
  CHECK(publication->after == publication->before + 1 && publication->epoch == s->epoch);
  CHECK(!s->cache.pending_count);
  CHECK(publication->fingerprint == lb_service_fingerprint(s, ns) && publication->records.size > 4);
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
static void create(lb_service *s) {
  char path[] = "/tmp/littlebear-batch-XXXXXX";
  int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
  CHECK(lb_service_create(s, path, 19, 1 << 20).code == lb_error_ok);
  unlink(path);
  s->changed = changed; s->changed_context = s;
  publications = notifications = 0;
}
static void begin(lb_task *t, lb_service *s, size_t count, size_t window, bool mixed) {
  lb_buffer b = payload(count, mixed);
  lb_namespace ns = {9, 2};
  CHECK(lb_task_init(t, s, 6, ns, true, s->epoch, lb_service_revision(s, ns), lb_buffer_bytes(&b)));
  t->write_limit = window;
  lb_buffer_free(&b);
  lb_task_step(t);
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
static void drive(lb_task *t) {
  size_t turns = 0;
  while (t->phase != lb_task_done) {
    CHECK(++turns < 1000 && !publications);
    /* Complete highest slots first; lower slots remain live across refills. */
    lb_store_request *chosen = NULL;
    lb_store_io_operation operation = {0};
    for (size_t i = 0; i <= t->extra_count; ++i) {
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
    lb_task_step(t);
  }
  CHECK(publications == 1 && !t->locked && t->succeeded);
  if (t->no_ack) CHECK(!t->reply.data && !t->reply.size && !t->reply.capacity);
  else CHECK(t->reply.size == 17 && t->reply.data[0] == 0);
}
static void overlap_and_partial(void) {
  lb_service s = {0}; lb_task t = {0}; create(&s);
  begin(&t, &s, 40, 4, false);
  CHECK(t.extra_count == 3 && t.position == 4 && s.cache.pending_count == 4);
  CHECK(t.locked && !publications);
  lb_store_io_operation op = submit(&t.io);
  CHECK(lb_store_complete_io(&t.io, -EAGAIN).code == lb_error_more_io);
  CHECK(t.io.state == lb_phase_ready && s.cache.pending_count == 4);
  op = submit(&t.io);
  CHECK(pwrite(op.fd, op.buffer, 5, (off_t)op.offset) == 5);
  CHECK(lb_store_complete_io(&t.io, 5).code == lb_error_more_io);
  CHECK(t.io.transferred == 5 && !publications);
  /* Grow/repack namespace metadata while all request pointers stay pinned. */
  for (uint64_t i = 100; i < 500; ++i)
    CHECK(lb_store_create_namespace(&s.cache, (lb_namespace){i, 3}).code == lb_error_ok);
  drive(&t);
  CHECK(lb_index_summary(lb_directory_get(&s.cache.namespaces, t.ns),
                        (ct_range){0, 0, false}).count == 40);
  lb_task_dispose(&t); lb_service_destroy(&s);
}
static void failure_drains(void) {
  lb_service s = {0}; lb_task t = {0}; create(&s);
  begin(&t, &s, 12, 4, false);
  lb_store_io_operation a = submit(&t.io), b = submit(&t.extra_io[0]);
  (void)a;
  CHECK(lb_store_complete_io(&t.io, -EIO).code == lb_error_io_failure);
  lb_task_step(&t);
  CHECK(t.abort_status == 2 && t.locked && t.phase != lb_task_done);
  CHECK(t.position == 4 && s.cache.pending_count == 1 && !publications);
  CHECK(!t.extra_io[1].owner && !t.extra_io[2].owner);
  finish_io(&t.extra_io[0], b);
  lb_task_step(&t);
  CHECK(t.phase == lb_task_done && t.reply.data[0] == 2 && !t.locked);
  CHECK(!publications && notifications == 1 && !s.cache.pending_count);
  lb_task_dispose(&t); lb_service_destroy(&s);
}
static void capacity_and_cancel(void) {
  lb_service s = {0}; lb_task t = {0}; lb_store_request blocker = {0}; create(&s);
  s.cache.pending_limit = 3;
  lb_namespace other = {77, 1};
  CHECK(lb_store_create_namespace(&s.cache, other).code == lb_error_ok);
  CHECK(lb_store_prepare_write(&s.cache, &blocker, other, 1, "data", 4, 0).code == lb_error_ok);
  begin(&t, &s, 12, 4, false);
  CHECK(t.extra_count == 2 && t.position == 2 && t.waiting_capacity);
  CHECK(s.cache.pending_count == 3 && lb_task_io_ready(&t));
  CHECK(lb_store_cancel(&blocker).code == lb_error_cancelled);
  lb_task_step(&t);
  CHECK(!t.waiting_capacity && t.position == 3 && s.cache.pending_count == 3);
  drive(&t);
  lb_task_dispose(&t);
  publications = 0;
  begin(&t, &s, 12, 4, false);
  CHECK(s.cache.pending_count == 3);
  lb_task_dispose(&t);
  CHECK(!s.cache.pending_count && !publications && !lb_service_writing(&s, (lb_namespace){9, 2}));
  lb_service_destroy(&s);
}
static void mixed_changes(void) {
  lb_service s = {0}; lb_task t = {0}; create(&s);
  begin(&t, &s, 24, 4, false); drive(&t); lb_task_dispose(&t);
  publications = 0;
  begin(&t, &s, 24, 4, true); drive(&t);
  CHECK(lb_index_summary(lb_directory_get(&s.cache.namespaces, t.ns),
                        (ct_range){0, 0, false}).count == 16);
  lb_task_dispose(&t); lb_service_destroy(&s);
}
static void mismatch_and_retry_drain(void) {
  lb_service s = {0}; lb_task t = {0}; create(&s);
  begin(&t, &s, 4, 4, false);
  t.expected ^= 1;
  for (size_t i = 0; i <= t.extra_count; ++i) {
    lb_store_request *r = lb_task_io(&t, i);
    finish_io(r, submit(r));
    CHECK(!publications && t.locked);
  }
  lb_task_step(&t);
  CHECK(t.phase == lb_task_done && t.reply.data[0] == 1 && !publications && !t.locked);
  lb_task_dispose(&t);
  begin(&t, &s, 4, 4, false);
  submit(&t.io); submit(&t.extra_io[0]);
  CHECK(lb_store_complete_io(&t.io, -EIO).code == lb_error_io_failure);
  lb_task_step(&t);
  CHECK(t.locked && t.abort_status == 2 && s.cache.pending_count == 1);
  /* A retryable completion after another write failed must be cancelled,
   * not resubmitted, and must release the final reservation and lock. */
  CHECK(lb_store_complete_io(&t.extra_io[0], -EAGAIN).code == lb_error_more_io);
  lb_task_step(&t);
  CHECK(t.phase == lb_task_done && t.reply.data[0] == 2 && !t.locked);
  CHECK(!publications && !s.cache.pending_count);
  lb_task_dispose(&t); lb_service_destroy(&s);
}
static void fair_share(void) {
  lb_service s = {0}; lb_task t = {0}, others[3] = {{0}}; create(&s);
  s.cache.pending_limit = 8;
  begin(&t, &s, 24, 4, false);
  lb_buffer b = payload(24, false);
  for (size_t i = 0; i < 3; ++i) {
    CHECK(lb_task_init(&others[i], &s, 6, (lb_namespace){20 + i, 2}, true,
                       s.epoch, 0, lb_buffer_bytes(&b)));
    others[i].write_limit = 4;
    lb_task_step(&others[i]);
  }
  lb_buffer_free(&b);
  CHECK(s.active_writers == 4 && s.cache.pending_count == 8);
  for (size_t i = 0; i < 4; ++i) {
    lb_store_io_operation op = submit(lb_task_io(&t, i));
    if (i < 3) finish_io(lb_task_io(&t, i), op);
  }
  lb_task_step(&t);
  /* One old operation remains; the shrunken share admits only one more. */
  CHECK(t.position == 5 && s.cache.pending_count == 6 && !publications);
  for (size_t i = 0; i < 3; ++i) lb_task_dispose(&others[i]);
  CHECK(s.active_writers == 1);
  lb_task_step(&t);
  CHECK(t.position == 7 && s.cache.pending_count == 4);
  drive(&t);
  CHECK(!s.active_writers);
  lb_task_dispose(&t); lb_service_destroy(&s);
}
static void no_ack_completion(void) {
  lb_service s = {0}; lb_task t = {0}; create(&s);
  begin(&t, &s, 4, 4, false);
  t.no_ack = true;
  drive(&t);
  CHECK(notifications == 1);
  lb_task_dispose(&t);
  CHECK(notifications == 1); /* Disposal cannot notify twice. */
  publications = 0;
  begin(&t, &s, 4, 4, false);
  t.no_ack = true;
  t.expected ^= 1;
  for (size_t i = 0; i <= t.extra_count; ++i) {
    lb_store_request *r = lb_task_io(&t, i);
    finish_io(r, submit(r));
  }
  lb_task_step(&t);
  CHECK(t.phase == lb_task_done && !t.succeeded && t.reply.data[0] == 1);
  CHECK(notifications == 2 && !publications);
  lb_task_dispose(&t);
  begin(&t, &s, 4, 4, false);
  t.no_ack = true;
  submit(&t.io);
  CHECK(lb_store_complete_io(&t.io, -EIO).code == lb_error_io_failure);
  lb_task_step(&t);
  CHECK(t.phase == lb_task_done && !t.succeeded && t.reply.data[0] == 2);
  CHECK(notifications == 3 && !publications);
  lb_task_dispose(&t); lb_service_destroy(&s);
}
int main(void) {
  overlap_and_partial(); failure_drains(); capacity_and_cancel(); mixed_changes();
  mismatch_and_retry_drain();
  fair_share();
  no_ack_completion();
  puts("PASS: concurrent batch writes, out-of-order refill, partial I/O, capacity, failure drain, cancellation and mixed changes");
  return 0;
}
