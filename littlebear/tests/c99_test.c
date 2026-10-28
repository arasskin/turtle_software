#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "sync.h"
#include <stdio.h>
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                      \
      abort();                                                                                     \
    }                                                                                              \
  } while (0)
static uint64_t random_state = UINT64_C(71829293);
static uint64_t random_word(void) {
  random_state ^= random_state << 13;
  random_state ^= random_state >> 7;
  random_state ^= random_state << 17;
  return random_state;
}
static void model(void) {
  ct_pool pool;
  lb_index index;
  ct_record expected[2048];
  bool present[2048] = {false};
  size_t step, i;
  ct_range all = {0, 0, false};
  ct_pool_init(&pool, NULL);
  index = lb_index_create(&pool);
  for (step = 0; step < 20000; ++step) {
    size_t k = (size_t)(random_word() % 2048);
    ct_record r = {k, random_word() | 1, random_word()};
    if (random_word() % 3) {
      CHECK(lb_index_set(&index, k, r.value, r.hash) >= 0);
      expected[k] = r;
      present[k] = true;
    } else {
      CHECK(lb_index_erase(&index, k) == (present[k] ? 1 : 0));
      present[k] = false;
    }
    if (step % 71 == 0) {
      ct_summary summary = lb_index_summary(&index, all);
      uint64_t count = 0, hash = 0;
      ct_tree tree = lb_index_view(&index);
      CHECK(ct_tree_validate(&tree));
      for (i = 0; i < 2048; ++i) {
        ct_record found;
        bool exists = lb_index_find(&index, i, &found);
        CHECK(exists == present[i]);
        if (exists) {
          CHECK(found.key == expected[i].key && found.value == expected[i].value &&
                found.hash == expected[i].hash);
          CHECK(lb_index_select(&index, count, &found) && found.key == i);
          ++count;
          hash ^= expected[i].hash;
        }
      }
      CHECK(summary.count == count && summary.hash == hash);
    }
  }
  {
    lb_recon_workspace work;
    uint8_t bytes[4096];
    lb_recon_output out = {bytes, sizeof(bytes), 0, 0};
    CHECK(lb_recon_begin(lb_index_source(&index), &work, &out) == lb_recon_status_complete);
    CHECK(out.size >= 4 && !memcmp(bytes, "LBR\3", 4));
  }
  lb_index_destroy(&index);
  CHECK(pool.stats.live_bytes == 0);
  ct_pool_destroy(&pool);
  CHECK(pool.stats.reserved_bytes == 0);
}
static lb_buffer batch(uint64_t id, const char *value) {
  lb_buffer fact = {0}, hashed = {0}, out = {0};
  lb_u64(&hashed, id);
  lb_append(&hashed, value, strlen(value));
  lb_u64(&fact, XXH64(hashed.data, hashed.size, 0));
  lb_append(&fact, value, strlen(value));
  lb_u32(&out, 1);
  lb_u64(&out, id);
  lb_u8(&out, 1);
  lb_blob(&out, lb_buffer_bytes(&fact));
  lb_buffer_free(&fact);
  lb_buffer_free(&hashed);
  return out;
}
static void sync_lifetime(void) {
  char path[] = "/tmp/littlebear-c99-XXXXXX";
  int fd = mkstemp(path);
  lb_service service = {0};
  lb_namespace ns = {1, 2};
  lb_task task;
  lb_buffer payload = batch(17, "first"), reply = {0}, fetch = {0};
  uint64_t first_revision;
  CHECK(fd >= 0);
  close(fd);
  CHECK(lb_service_create(&service, path, 77, 1 << 20).code == lb_error_ok);
  CHECK(lb_handle(&service, 0, ns, false, 0, 0, lb_bytes_make("\0", 1), &reply));
  CHECK(reply.data[0] == 3);
  lb_buffer_free(&reply);
  CHECK(lb_task_init(&task, &service, 3, ns, true, 77, 0, lb_buffer_bytes(&payload)));
  lb_buffer_free(&payload);
  lb_task_step(&task);
  CHECK(task.io.owner && task.io.state == lb_phase_ready && lb_service_writing(&service, ns));
  lb_task_dispose(&task);
  CHECK(!lb_service_writing(&service, ns));
  CHECK(lb_service_fingerprint(&service, ns) == 0);
  payload = batch(17, "second");
  CHECK(lb_handle(&service, 3, ns, true, 77, lb_service_revision(&service, ns),
                  lb_buffer_bytes(&payload), &reply));
  CHECK(reply.data[0] == 0);
  lb_buffer_free(&reply);
  lb_buffer_free(&payload);
  first_revision = lb_service_revision(&service, ns);
  lb_u32(&fetch, 1);
  lb_u64(&fetch, 17);
  CHECK(lb_task_init(&task, &service, 2, ns, false, 77, first_revision, lb_buffer_bytes(&fetch)));
  lb_task_step(&task);
  CHECK(task.io.owner);
  {
    lb_store_io_operation op = {0};
    ssize_t n;
    CHECK(lb_store_start_io(&task.io, &op).code == lb_error_ok);
    n = pread(op.fd, op.buffer, op.length, (off_t)op.offset);
    CHECK(n > 0);
    payload = batch(18, "other record");
    CHECK(lb_handle(&service, 3, ns, true, 77, first_revision, lb_buffer_bytes(&payload), &reply));
    CHECK(reply.data[0] == 0);
    lb_buffer_free(&reply);
    lb_buffer_free(&payload);
    CHECK(lb_store_complete_io(&task.io, (int)n).code == lb_error_ok);
    lb_task_step(&task);
    CHECK(task.phase == lb_task_done && task.reply.data[0] == 1 && task.reply.size > 17);
  }
  lb_task_dispose(&task);
  lb_buffer_free(&fetch);
  /* Entire batches validate before any mutation, including truncated facts. */
  first_revision = lb_service_revision(&service, ns);
  payload = batch(17, "invalid");
  --payload.size;
  CHECK(lb_handle(&service, 3, ns, true, 77, first_revision, lb_buffer_bytes(&payload), &reply));
  CHECK(reply.data[0] == 2 && lb_service_revision(&service, ns) == first_revision);
  lb_buffer_free(&payload);
  lb_buffer_free(&reply);
  lb_service_destroy(&service);
  unlink(path);
}
int main(void) {
  model();
  sync_lifetime();
  puts("PASS: C99 tree model, reconciliation, request ownership, cancellation, stale read restart "
       "and batch validation");
  return 0;
}
