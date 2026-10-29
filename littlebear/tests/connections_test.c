#define _POSIX_C_SOURCE 200809L
#include "client_connections.h"
#include "cache_driver.h"
#include <stdio.h>

static void test_subscribers_changed(void *context, lb_namespace ns, const lb_publication *p) {
  subscribers_changed(context, ns, p);
}
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
  buffer_pool buffers = CT_ZERO;
  lb_subscribers subscribers = CT_ZERO;
  connections s = CT_ZERO;
  s.buffers = &buffers; s.subscribers = &subscribers;
  subscribers.context = &s;
  subscribers.reserve = connection_delivery_reserve;
  subscribers.wake = connection_delivery_wake;
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
  buffers_trim(&buffers); assert(!buffers.allocated); ct_map_free(&subscribers.watches);
}
static void buffers_and_budget(void) {
  buffer_pool buffers = CT_ZERO;
  lb_subscribers subscribers = CT_ZERO;
  connections s = CT_ZERO;
  s.buffers = &buffers; s.subscribers = &subscribers;
  subscribers.context = &s;
  subscribers.reserve = connection_delivery_reserve;
  subscribers.wake = connection_delivery_wake;
  lb_buffer a = CT_ZERO, b = CT_ZERO;
  s.memory_budget = 32 * 1024 * 1024;
  assert(buffer_borrow(&s, &a, 100));
  uint8_t *original = a.data;
  assert(a.capacity == 128);
  buffer_return(s.buffers, &a);
  assert(!a.data && s.buffers->cached == 128);
  assert(buffer_borrow(&s, &b, 100) && b.data == original);
  buffer_return(s.buffers, &b);
  assert(buffer_borrow(&s, &a, LB_MAX_FRAME + 4));
  assert(a.capacity == LB_MAX_FRAME + 4);
  buffer_return(s.buffers, &a);
  assert(s.buffers->cached == 128);
  s.memory_budget = 64;
  assert(!buffer_borrow(&s, &a, 100) && !s.buffers->allocated);
  assert(buffer_borrow(&s, &a, 64));
  assert(!buffer_borrow(&s, &b, 64));
  buffer_return(s.buffers, &a);
  connections_destroy(&s);
  buffers_trim(&buffers); assert(!buffers.allocated); ct_map_free(&subscribers.watches);

  s.buffers = &buffers; s.subscribers = &subscribers;
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
  buffers_trim(&buffers); assert(!buffers.allocated); ct_map_free(&subscribers.watches);
}
static void subscriptions(void) {
  buffer_pool buffers = CT_ZERO;
  lb_subscribers subscribers = CT_ZERO;
  connections s = CT_ZERO;
  s.buffers = &buffers; s.subscribers = &subscribers;
  subscribers.context = &s;
  subscribers.reserve = connection_delivery_reserve;
  subscribers.wake = connection_delivery_wake;
  channel channels[3] = CT_ZERO;
  lb_namespace a = {11, 22}, collision = {11, 33};
  s.memory_budget = 32 * 1024 * 1024;
  client *c = connection_take(&s), *d = connection_take(&s);
  assert(c && d);
  for (size_t i = 0; i < 3; ++i) {
    channels[i].id = (uint32_t)i + 100;
    memcpy(channels[i].name, i == 2 ? &collision : &a, 16);
    assert(subscription_add(s.subscribers, i == 1 ? &d->peer : &c->peer, &channels[i]));
    subscription_clean(channels[i].subscription);
  }
  queue_remove(c); queue_remove(d);
  assert(s.subscribers->watch_count == 2);
  subscribers_changed(s.subscribers, a, NULL);
  subscribers_changed(s.subscribers, a, NULL); /* coalesced dirty entries and runnable clients */
  assert(s.ready.head && s.ready.head->queue_next && !s.ready.head->queue_next->queue_next);
  assert(c->peer.dirty_head == channels[0].subscription && !c->peer.dirty_head->dirty_next);
  assert(d->peer.dirty_head == channels[1].subscription && !d->peer.dirty_head->dirty_next);
  assert(!channels[2].subscription->dirty);
  channel moved = channels[0];
  memset(&channels[0], 0, sizeof(channels[0]));
  assert(moved.subscription->channel_id == 100); /* no pointer into movable array */
  subscription_remove(s.subscribers, moved.subscription);
  subscription_remove(s.subscribers, channels[1].subscription);
  assert(s.subscribers->watch_count == 1 && !c->peer.dirty_head && !d->peer.dirty_head);
  subscribers_changed(s.subscribers, collision, NULL);
  assert(c->peer.dirty_head == channels[2].subscription);
  subscription_remove(s.subscribers, channels[2].subscription);
  assert(!s.subscribers->watches.size && !s.subscribers->watch_count && !s.subscribers->subscription_count);
  connection_release(&s, c); connection_release(&s, d);
  connections_destroy(&s);
  buffers_trim(&buffers); assert(!buffers.allocated); ct_map_free(&subscribers.watches);
}
static void task_yield_and_unlock(void) {
  char path[] = "/tmp/littlebear-connections-XXXXXX";
  int fd = mkstemp(path);
  lb_test_cache service = CT_ZERO;
  buffer_pool buffers = CT_ZERO;
  lb_subscribers subscribers = CT_ZERO;
  connections s = CT_ZERO;
  s.buffers = &buffers; s.subscribers = &subscribers;
  subscribers.context = &s;
  subscribers.reserve = connection_delivery_reserve;
  subscribers.wake = connection_delivery_wake;
  lb_task task = CT_ZERO;
  lb_buffer payload = CT_ZERO;
  channel ch = CT_ZERO;
  lb_namespace ns = {17, 23};
  assert(fd >= 0);
  close(fd);
  assert(lb_test_cache_create(&service, path, 5, 1024 * 1024).code == lb_error_ok);
  unlink(path);
  s.memory_budget = 16 * 1024 * 1024;
  service.changed = test_subscribers_changed;
  service.changed_context = s.subscribers;
  client *c = connection_take(&s);
  assert(c);
  memcpy(ch.name, &ns, 16);
  ch.id = 7;
  /* The publication path resolves subscriptions through the channel index. */
  c->peer.channels = &ch;
  c->peer.channel_count = 1;
  assert(ct_map_put(&c->peer.channel_index, ch.id, 0));
  assert(subscription_add(s.subscribers, &c->peer, &ch));
  subscription_clean(ch.subscription);
  queue_remove(c);
  lb_u32(&payload, 193);
  for (uint64_t i = 0; i < 193; ++i) {
    lb_u64(&payload, i);
    lb_u8(&payload, 0); /* synchronous deletion of absent records */
  }
  assert(lb_test_task_prepare(&task, 3, ns, true, 5, 0, lb_buffer_bytes(&payload)));
  lb_test_task_step_budget(&task, &service, 64);
  assert(lb_test_write_batch(&task)->position == 64 && lb_test_write_locked(&task) && task.phase != lb_task_done);
  assert(!c->peer.dirty_head && !c->queue);
  lb_test_task_dispose(&task, &service); /* aborted write unlock must wake its subscribers */
  assert(c->peer.dirty_head == ch.subscription && c->queue == &s.ready);
  subscription_clean(ch.subscription);
  queue_remove(c);
  assert(lb_test_task_prepare(&task, 3, ns, true, 5,
                      lb_test_cache_revision(&service, ns), lb_buffer_bytes(&payload)));
  for (size_t i = 1; i <= 3; ++i) {
    lb_test_task_step_budget(&task, &service, 64);
    assert(lb_test_write_batch(&task)->position == i * 64 && lb_test_write_locked(&task) && !c->peer.dirty_head);
  }
  lb_test_task_step_budget(&task, &service, 64);
  assert(task.phase == lb_task_done && !lb_test_write_locked(&task) && c->peer.dirty_head);
  lb_test_task_dispose(&task, &service);
  lb_buffer_free(&payload);
  subscription_remove(s.subscribers, ch.subscription);
  ct_map_free(&c->peer.channel_index);
  c->peer.channels = NULL;
  c->peer.channel_count = 0;
  connection_release(&s, c);
  connections_destroy(&s);
  buffers_trim(&buffers); assert(!buffers.allocated); ct_map_free(&subscribers.watches);
  lb_test_cache_destroy(&service);
}
static void capacity_fifo(void) {
  buffer_pool buffers = CT_ZERO;
  lb_subscribers subscribers = CT_ZERO;
  connections s = CT_ZERO;
  s.buffers = &buffers; s.subscribers = &subscribers;
  subscribers.context = &s;
  subscribers.reserve = connection_delivery_reserve;
  subscribers.wake = connection_delivery_wake;
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
  buffers_trim(&buffers); assert(!buffers.allocated); ct_map_free(&subscribers.watches);
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
  lb_test_cache service = CT_ZERO;
  lb_task writer = CT_ZERO, reader = CT_ZERO, other = CT_ZERO;
  lb_store_request blocker = CT_ZERO, conflict = CT_ZERO;
  lb_namespace a = {41, 1}, b = {42, 1};
  lb_buffer payload = write_payload(), ids = CT_ZERO, reply = CT_ZERO;
  char read_buffer[64];
  assert(fd >= 0);
  close(fd);
  assert(lb_test_cache_create(&service, path, 5, 1024 * 1024).code == lb_error_ok);
  unlink(path);
  service.cache.pending_limit = 1;
  assert(lb_store_create_namespace(&service.cache, a).code == lb_error_ok);
  assert(lb_store_prepare_write(&service.cache, &blocker, a, 1, "block", 5, 0).code == lb_error_ok);
  assert(lb_store_prepare_read(&service.cache, &conflict, a, 1, read_buffer, 64).code == lb_error_busy);
  assert(lb_test_task_prepare(&writer, 3, b, true, 5, 0, lb_buffer_bytes(&payload)));
  lb_test_task_step(&writer, &service);
  assert(writer.waiting_capacity && lb_test_write_locked(&writer) && lb_test_write_batch(&writer)->position == 0 && !writer.pending.first.owner);
  assert(writer.phase == lb_task_active && !writer.reply.size && service.cache.pending_count == 1);
  uint64_t version = writer.version;
  assert(lb_store_cancel(&blocker).code == lb_error_cancelled);
  lb_test_task_step(&writer, &service);
  assert(!writer.waiting_capacity && writer.pending.first.owner && writer.version == version);
  assert(lb_test_perform(&writer.pending.first).code == lb_error_ok);
  /* Exhaust capacity between records: resume must not skip/reapply record 1. */
  assert(lb_store_prepare_write(&service.cache, &blocker, a, 1, "block", 5, 0).code == lb_error_ok);
  lb_test_task_step(&writer, &service);
  assert(writer.waiting_capacity && lb_test_write_batch(&writer)->position == 1 && lb_test_write_locked(&writer) && !writer.reply.size);
  assert(lb_store_cancel(&blocker).code == lb_error_cancelled);
  lb_test_task_step(&writer, &service);
  assert(writer.pending.first.owner && writer.pending.first.key == 2);
  assert(lb_test_perform(&writer.pending.first).code == lb_error_ok);
  lb_test_task_step(&writer, &service);
  assert(writer.phase == lb_task_done && !lb_test_write_locked(&writer) && writer.reply.data[0] == 0);
  lb_test_task_dispose(&writer, &service);
  assert(lb_index_summary(lb_directory_get(&service.cache.namespaces, b),
                          (ct_range){0, 0, false}).count == 2);

  lb_u32(&ids, 2); lb_u64(&ids, 1); lb_u64(&ids, 2);
  assert(lb_store_prepare_write(&service.cache, &blocker, a, 1, "block", 5, 0).code == lb_error_ok);
  assert(lb_test_task_prepare(&reader, 2, b, false, 5, version, lb_buffer_bytes(&ids)));
  lb_test_task_step(&reader, &service);
  assert(reader.waiting_capacity && reader.phase == lb_task_active && ((lb_fetch_records_state *)reader.state)->position == 0);
  /* A different writer changes the namespace while the read is parked. */
  assert(lb_test_task_prepare(&other, 3, b, true, 5, version, lb_buffer_bytes(&payload)));
  lb_test_task_step(&other, &service);
  assert(other.waiting_capacity && lb_test_write_locked(&other));
  assert(lb_store_cancel(&blocker).code == lb_error_cancelled);
  lb_test_task_step(&reader, &service);
  assert(reader.phase == lb_task_done && reader.reply.data[0] == 1 && !reader.pending.first.owner);
  lb_test_task_dispose(&reader, &service);
  lb_test_task_dispose(&other, &service); /* disconnect while waiting releases the namespace lock */
  assert(!lb_test_cache_writing(&service, b) && !service.cache.pending_count);

  /* The synchronous API cannot wait on an externally owned request. */
  assert(lb_store_prepare_write(&service.cache, &blocker, a, 1, "block", 5, 0).code == lb_error_ok);
  assert(lb_test_handle(&service, 3, b, true, 5, lb_test_cache_revision(&service, b),
                   lb_buffer_bytes(&payload), &reply));
  assert(reply.data[0] == 1 && !lb_test_cache_writing(&service, b));
  assert(lb_store_cancel(&blocker).code == lb_error_cancelled);
  lb_buffer_free(&reply); lb_buffer_free(&ids); lb_buffer_free(&payload);
  lb_test_cache_destroy(&service);
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
