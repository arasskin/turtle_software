#define _POSIX_C_SOURCE 200809L
#include "connections.h"
#include <stdio.h>

static uint64_t random_state = 71;
static uint64_t random_value(void) {
  random_state = random_state * UINT64_C(6364136223846793005) + 1;
  return random_state >> 16;
}
static void check_timers(connections *s) {
  for (size_t i = 0; i < s->timer_count; ++i) {
    assert(s->timers[i]->timer_index == i);
    if (i) assert(s->timers[(i - 1) / 2]->deadline <= s->timers[i]->deadline);
  }
}
static void pool_and_timers(void) {
  connections s = CT_ZERO;
  client *clients[1031];
  s.memory_budget = 32 * 1024 * 1024;
  for (size_t i = 0; i < 1031; ++i) {
    clients[i] = connection_take(&s);
    assert(clients[i]);
    clients[i]->fd = (int)i;
    timer_touch(&s, clients[i], random_value() % 100000);
    queue_push(&s.ready, clients[i]);
    queue_push(&s.ready, clients[i]);
  }
  assert(s.count == 1031 && s.slab_count == 9);
  for (size_t i = 0; i < 1031; ++i) {
    assert(clients[i]->receive.owner == clients[i]);
    assert(queue_pop(&s.ready) == clients[i]);
  }
  assert(!queue_pop(&s.ready));
  for (size_t i = 0; i < 10000; ++i) {
    client *c = clients[random_value() % 1031];
    if (random_value() % 3) timer_touch(&s, c, random_value() % 100000);
    else timer_remove(&s, c);
    check_timers(&s);
  }
  for (size_t i = 0; i < 1031; ++i) {
    client *c = clients[i];
    queue_push(&s.ready, c);
    queue_push(&s.submission_wait, c);
    queue_push(&s.ready, c);
    connection_release(&s, c);
    client *reused = connection_take(&s);
    assert(reused == c && reused->timer_index == SIZE_MAX && !reused->queue);
    assert(reused->receive.owner == reused && !reused->closing);
    connection_release(&s, reused);
    check_timers(&s);
  }
  assert(!s.count && !s.timer_count && !s.ready.head && !s.submission_wait.head);
  connections_destroy(&s);
}
static void buffers_and_budget(void) {
  connections s = CT_ZERO;
  lb_buffer a = CT_ZERO, b = CT_ZERO;
  s.memory_budget = 32 * 1024 * 1024;
  assert(buffer_borrow(&s, &a, 100));
  uint8_t *original = a.data;
  assert(a.capacity == 128);
  buffer_return(&s.buffers, &a);
  assert(!a.data && s.buffers.cached == 128);
  assert(buffer_borrow(&s, &b, 100) && b.data == original);
  buffer_return(&s.buffers, &b);
  assert(buffer_borrow(&s, &a, LB_MAX_FRAME + 4));
  assert(a.capacity == LB_MAX_FRAME + 4);
  buffer_return(&s.buffers, &a);
  assert(s.buffers.cached == 128);
  s.memory_budget = 64;
  assert(!buffer_borrow(&s, &a, 100) && !s.buffers.allocated);
  assert(buffer_borrow(&s, &a, 64));
  assert(!buffer_borrow(&s, &b, 64));
  buffer_return(&s.buffers, &a);
  connections_destroy(&s);

  s.memory_budget = sizeof(client_slab) + 8 * sizeof(client *);
  client *c = connection_take(&s);
  assert(c);
  size_t used = connection_memory(&s);
  s.memory_budget = used;
  client *others[7];
  for (size_t i = 0; i < 7; ++i) assert((others[i] = connection_take(&s)));
  assert(!connection_take(&s)); /* no budget for timer growth */
  connection_release(&s, c);
  for (size_t i = 0; i < 7; ++i) connection_release(&s, others[i]);
  connections_destroy(&s);
}
static void subscriptions(void) {
  connections s = CT_ZERO;
  channel channels[3] = CT_ZERO;
  lb_namespace a = {11, 22}, collision = {11, 33};
  s.memory_budget = 32 * 1024 * 1024;
  client *c = connection_take(&s), *d = connection_take(&s);
  assert(c && d);
  for (size_t i = 0; i < 3; ++i) {
    channels[i].id = (uint32_t)i + 100;
    memcpy(channels[i].name, i == 2 ? &collision : &a, 16);
    assert(subscription_add(&s, i == 1 ? d : c, &channels[i]));
    subscription_clean(channels[i].subscription);
  }
  queue_remove(c); queue_remove(d);
  assert(s.watch_count == 2);
  connections_changed(&s, a, NULL);
  connections_changed(&s, a, NULL); /* coalesced dirty entries and runnable clients */
  assert(s.ready.head && s.ready.head->queue_next && !s.ready.head->queue_next->queue_next);
  assert(c->dirty_head == channels[0].subscription && !c->dirty_head->dirty_next);
  assert(d->dirty_head == channels[1].subscription && !d->dirty_head->dirty_next);
  assert(!channels[2].subscription->dirty);
  channel moved = channels[0];
  memset(&channels[0], 0, sizeof(channels[0]));
  assert(moved.subscription->channel_id == 100); /* no pointer into movable array */
  subscription_remove(&s, moved.subscription);
  subscription_remove(&s, channels[1].subscription);
  assert(s.watch_count == 1 && !c->dirty_head && !d->dirty_head);
  connections_changed(&s, collision, NULL);
  assert(c->dirty_head == channels[2].subscription);
  subscription_remove(&s, channels[2].subscription);
  assert(!s.watches.size && !s.watch_count && !s.subscription_count);
  connection_release(&s, c); connection_release(&s, d);
  connections_destroy(&s);
}
static void task_yield_and_unlock(void) {
  char path[] = "/tmp/littlebear-connections-XXXXXX";
  int fd = mkstemp(path);
  lb_service service = CT_ZERO;
  connections s = CT_ZERO;
  lb_task task = CT_ZERO;
  lb_buffer payload = CT_ZERO;
  channel ch = CT_ZERO;
  lb_namespace ns = {17, 23};
  assert(fd >= 0);
  close(fd);
  assert(lb_service_create(&service, path, 5, 1024 * 1024).code == lb_error_ok);
  unlink(path);
  s.memory_budget = 16 * 1024 * 1024;
  service.changed = connections_changed;
  service.changed_context = &s;
  client *c = connection_take(&s);
  assert(c);
  memcpy(ch.name, &ns, 16);
  ch.id = 7;
  /* The publication path resolves subscriptions through the channel index. */
  c->channels = &ch;
  c->channel_count = 1;
  assert(ct_map_put(&c->channel_index, ch.id, 0));
  assert(subscription_add(&s, c, &ch));
  subscription_clean(ch.subscription);
  queue_remove(c);
  lb_u32(&payload, 193);
  for (uint64_t i = 0; i < 193; ++i) {
    lb_u64(&payload, i);
    lb_u8(&payload, 0); /* synchronous deletion of absent records */
  }
  assert(lb_task_init(&task, &service, 3, ns, true, 5, 0, lb_buffer_bytes(&payload)));
  lb_task_step_budget(&task, 64);
  assert(task.position == 64 && task.locked && task.phase != lb_task_done);
  assert(!c->dirty_head && !c->queue);
  lb_task_dispose(&task); /* aborted write unlock must wake its subscribers */
  assert(c->dirty_head == ch.subscription && c->queue == &s.ready);
  subscription_clean(ch.subscription);
  queue_remove(c);
  assert(lb_task_init(&task, &service, 3, ns, true, 5,
                      lb_service_revision(&service, ns), lb_buffer_bytes(&payload)));
  for (size_t i = 1; i <= 3; ++i) {
    lb_task_step_budget(&task, 64);
    assert(task.position == i * 64 && task.locked && !c->dirty_head);
  }
  lb_task_step_budget(&task, 64);
  assert(task.phase == lb_task_done && !task.locked && c->dirty_head);
  lb_task_dispose(&task);
  lb_buffer_free(&payload);
  subscription_remove(&s, ch.subscription);
  ct_map_free(&c->channel_index);
  c->channels = NULL;
  c->channel_count = 0;
  connection_release(&s, c);
  connections_destroy(&s);
  lb_service_destroy(&service);
}
static void capacity_fifo(void) {
  connections s = CT_ZERO;
  lb_store store = CT_ZERO;
  s.memory_budget = 16 * 1024 * 1024;
  client *a = connection_take(&s), *b = connection_take(&s), *fresh = connection_take(&s);
  assert(a && b && fresh);
  store.pending_limit = store.pending_count = 1;
  storage_wait_push(&s, a);
  storage_wait_push(&s, b);
  storage_wait_push(&s, a); /* idempotent */
  queue_push(&s.ready, b); /* unrelated notification preserves FIFO position */
  assert(connection_next(&s, &store) == b && b->storage_queued);
  queue_push(&s.ready, fresh);
  store.pending_count = 0;
  assert(connection_next(&s, &store) == a);
  store.pending_count = 1; /* oldest waiter consumes the only slot */
  assert(connection_next(&s, &store) == fresh);
  assert(!connection_next(&s, &store)); /* no runnable busy polling */
  queue_push(&s.ready, b); /* timeout/close can run despite full storage */
  assert(connection_next(&s, &store) == b);
  storage_wait_remove(&s, b);
  assert(!s.storage_head && !s.storage_tail && !s.storage_wait_count);
  storage_wait_push(&s, a);
  storage_wait_push(&s, b);
  storage_wait_push(&s, fresh);
  connection_release(&s, b); /* cancellation in the middle */
  assert(s.storage_head == a && a->storage_next == fresh && fresh->storage_prev == a);
  connection_release(&s, a);
  connection_release(&s, fresh);
  assert(!s.storage_wait_count);
  connections_destroy(&s);
}
static lb_buffer write_payload(void) {
  lb_buffer payload = CT_ZERO;
  lb_u32(&payload, 2);
  for (uint64_t id = 1; id <= 2; ++id) {
    lb_buffer hashed = CT_ZERO, value = CT_ZERO;
    lb_u64(&hashed, id);
    lb_append(&hashed, "test", 4);
    lb_u64(&value, XXH64(hashed.data, hashed.size, 0));
    lb_append(&value, "test", 4);
    lb_u64(&payload, id);
    lb_u8(&payload, 1);
    lb_blob(&payload, lb_buffer_bytes(&value));
    lb_buffer_free(&hashed);
    lb_buffer_free(&value);
  }
  return payload;
}
static void capacity_resume(void) {
  char path[] = "/tmp/littlebear-capacity-XXXXXX";
  int fd = mkstemp(path);
  lb_service service = CT_ZERO;
  lb_task writer = CT_ZERO, reader = CT_ZERO, other = CT_ZERO;
  lb_store_request blocker = CT_ZERO, conflict = CT_ZERO;
  lb_namespace a = {41, 1}, b = {42, 1};
  lb_buffer payload = write_payload(), ids = CT_ZERO, reply = CT_ZERO;
  char read_buffer[64];
  assert(fd >= 0);
  close(fd);
  assert(lb_service_create(&service, path, 5, 1024 * 1024).code == lb_error_ok);
  unlink(path);
  service.cache.pending_limit = 1;
  assert(lb_store_create_namespace(&service.cache, a).code == lb_error_ok);
  assert(lb_store_prepare_write(&service.cache, &blocker, a, 1, "block", 5, 0).code == lb_error_ok);
  assert(lb_store_prepare_read(&service.cache, &conflict, a, 1, read_buffer, 64).code == lb_error_busy);
  assert(lb_task_init(&writer, &service, 3, b, true, 5, 0, lb_buffer_bytes(&payload)));
  lb_task_step(&writer);
  assert(writer.waiting_capacity && writer.locked && writer.position == 0 && !writer.io.owner);
  assert(writer.phase == lb_task_write && !writer.reply.size && service.cache.pending_count == 1);
  uint64_t version = writer.version;
  assert(lb_store_cancel(&blocker).code == lb_error_cancelled);
  lb_task_step(&writer);
  assert(!writer.waiting_capacity && writer.io.owner && writer.version == version);
  assert(lb_perform(&writer.io).code == lb_error_ok);
  /* Exhaust capacity between records: resume must not skip/reapply record 1. */
  assert(lb_store_prepare_write(&service.cache, &blocker, a, 1, "block", 5, 0).code == lb_error_ok);
  lb_task_step(&writer);
  assert(writer.waiting_capacity && writer.position == 1 && writer.locked && !writer.reply.size);
  assert(lb_store_cancel(&blocker).code == lb_error_cancelled);
  lb_task_step(&writer);
  assert(writer.io.owner && writer.io.key == 2);
  assert(lb_perform(&writer.io).code == lb_error_ok);
  lb_task_step(&writer);
  assert(writer.phase == lb_task_done && !writer.locked && writer.reply.data[0] == 0);
  lb_task_dispose(&writer);
  assert(lb_index_summary(lb_directory_get(&service.cache.namespaces, b),
                          (ct_range){0, 0, false}).count == 2);

  lb_u32(&ids, 2); lb_u64(&ids, 1); lb_u64(&ids, 2);
  assert(lb_store_prepare_write(&service.cache, &blocker, a, 1, "block", 5, 0).code == lb_error_ok);
  assert(lb_task_init(&reader, &service, 2, b, false, 5, version, lb_buffer_bytes(&ids)));
  lb_task_step(&reader);
  assert(reader.waiting_capacity && reader.phase == lb_task_read && reader.position == 0);
  /* A different writer changes the namespace while the read is parked. */
  assert(lb_task_init(&other, &service, 3, b, true, 5, version, lb_buffer_bytes(&payload)));
  lb_task_step(&other);
  assert(other.waiting_capacity && other.locked);
  assert(lb_store_cancel(&blocker).code == lb_error_cancelled);
  lb_task_step(&reader);
  assert(reader.phase == lb_task_done && reader.reply.data[0] == 1 && !reader.io.owner);
  lb_task_dispose(&reader);
  lb_task_dispose(&other); /* disconnect while waiting releases the namespace lock */
  assert(!lb_service_writing(&service, b) && !service.cache.pending_count);

  /* The synchronous API cannot wait on an externally owned request. */
  assert(lb_store_prepare_write(&service.cache, &blocker, a, 1, "block", 5, 0).code == lb_error_ok);
  assert(lb_handle(&service, 3, b, true, 5, lb_service_revision(&service, b),
                   lb_buffer_bytes(&payload), &reply));
  assert(reply.data[0] == 1 && !lb_service_writing(&service, b));
  assert(lb_store_cancel(&blocker).code == lb_error_cancelled);
  lb_buffer_free(&reply); lb_buffer_free(&ids); lb_buffer_free(&payload);
  lb_service_destroy(&service);
}
int main(void) {
  pool_and_timers();
  buffers_and_budget();
  subscriptions();
  task_yield_and_unlock();
  capacity_fifo();
  capacity_resume();
  puts("PASS: stable connection slabs, reuse, deduplicated queues, 10,000 timer mutations, pooled buffers/budget, targeted subscriptions and bounded tasks with abort/completion wakeups");
  return 0;
}
