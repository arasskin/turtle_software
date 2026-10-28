#ifndef LB_CONNECTIONS_H
#define LB_CONNECTIONS_H
/* Portable connection ownership and scheduling. One event-loop thread owns all
 * lists. Slabs never move: kernel completion tags point into their slots. */
#include "../containers/map.h"
#include "auth.h"
#include "sync.h"
#include <sys/socket.h>
#include <sys/uio.h>

typedef struct client client;
typedef struct subscription subscription;
typedef struct watch watch;
typedef struct shared_update {
  size_t refs, size;
  uint64_t before, after;
  uint8_t data[]; /* status, epoch, revision, fingerprint, records */
} shared_update;
typedef struct delivery {
  struct delivery *next;
  shared_update *update;
} delivery;
enum { LB_DELIVERY_LIMIT = 16 * 1024 * 1024 };
typedef enum {
  op_accept, op_receive, op_send, op_file, op_cancel_receive, op_cancel_send,
  op_cancel_accept
} operation;
typedef struct completion {
  operation kind;
  client *owner;
  lb_store_request *io;
} completion;
typedef struct channel {
  uint32_t id;
  uint8_t name[16];
  lb_auth auth;
  uint64_t announced;
  bool subscribed, known, optimistic, needs_reconcile;
  subscription *subscription;
} channel;
typedef struct client_queue {
  client *head, *tail;
} client_queue;
struct client {
  int fd;
  bool closing, receiving, sending, cancelling_receive, cancelling_send,
      cancelled_receive, cancelled_send;
  bool body, task_active;
  uint8_t header[4];
  size_t received, sent;
  lb_buffer input, output;
  shared_update *output_update;
  uint8_t output_header[12];
  struct iovec output_vectors[2];
  struct msghdr output_message;
  size_t delivery_bytes;
  lb_task task;
  channel *channels;
  size_t channel_count, channel_capacity, active;
  ct_map channel_index;
  uint32_t request_id, channel_id;
  uint8_t op;
  completion receive, send, file, cancel_receive, cancel_send;
  completion *extra_files;
  size_t file_capacity, file_pending;
  uint64_t deadline;
  size_t timer_index, accounted;
  client *free_next, *live_prev, *live_next, *queue_prev, *queue_next;
  client_queue *queue;
  /* Separate links: notifications and timeout/close wakeups must not reorder
   * the capacity FIFO or let a queued writer bypass older waiters. */
  client *storage_prev, *storage_next;
  bool storage_queued;
  subscription *dirty_head, *dirty_tail;
};
enum { LB_CLIENT_SLAB = 128, LB_BUFFER_CLASSES = 19 };
typedef struct client_slab {
  struct client_slab *next;
  client slots[LB_CLIENT_SLAB];
} client_slab;
typedef struct buffer_link { struct buffer_link *next; } buffer_link;
typedef struct buffer_pool {
  buffer_link *bins[LB_BUFFER_CLASSES];
  size_t allocated, cached;
} buffer_pool;
struct watch {
  lb_namespace ns;
  watch *collision_next;
  subscription *head;
};
struct subscription {
  client *owner;
  uint32_t channel_id;
  watch *watch;
  subscription *prev, *next, *dirty_prev, *dirty_next;
  bool dirty;
  bool resync;
  delivery *head, *tail;
};
typedef struct connections {
  client_slab *slabs;
  client *free, *live;
  size_t count, slab_count, client_bytes, subscription_count, watch_count;
  client_queue ready, submission_wait;
  client *storage_head, *storage_tail;
  size_t storage_wait_count;
  client **timers;
  size_t timer_count, timer_capacity;
  ct_map watches;
  buffer_pool buffers;
  size_t memory_budget;
  size_t update_bytes, delivery_count;
} connections;

static inline size_t client_output_size(const client *c) {
  return c->output_update ? sizeof(c->output_header) + c->output_update->size : c->output.size;
}

static inline void client_init(client *c) {
  memset(c, 0, sizeof(*c));
  c->fd = -1;
  c->active = c->timer_index = SIZE_MAX;
  c->receive = (completion){op_receive, c, NULL};
  c->send = (completion){op_send, c, NULL};
  c->file = (completion){op_file, c, NULL};
  c->cancel_receive = (completion){op_cancel_receive, c, NULL};
  c->cancel_send = (completion){op_cancel_send, c, NULL};
}
static inline void queue_remove(client *c) {
  client_queue *q = c->queue;
  if (!q) return;
  if (c->queue_prev) c->queue_prev->queue_next = c->queue_next;
  else q->head = c->queue_next;
  if (c->queue_next) c->queue_next->queue_prev = c->queue_prev;
  else q->tail = c->queue_prev;
  c->queue = NULL;
  c->queue_prev = c->queue_next = NULL;
}
static inline void queue_push(client_queue *q, client *c) {
  if (c->queue == q) return;
  queue_remove(c);
  c->queue = q;
  c->queue_prev = q->tail;
  if (q->tail) q->tail->queue_next = c;
  else q->head = c;
  q->tail = c;
}
static inline client *queue_pop(client_queue *q) {
  client *c = q->head;
  if (c) queue_remove(c);
  return c;
}
static inline void storage_wait_remove(connections *s, client *c) {
  if (!c->storage_queued) return;
  if (c->storage_prev) c->storage_prev->storage_next = c->storage_next;
  else s->storage_head = c->storage_next;
  if (c->storage_next) c->storage_next->storage_prev = c->storage_prev;
  else s->storage_tail = c->storage_prev;
  c->storage_prev = c->storage_next = NULL;
  c->storage_queued = false;
  --s->storage_wait_count;
}
static inline void storage_wait_push(connections *s, client *c) {
  queue_remove(c);
  if (c->storage_queued) return;
  c->storage_queued = true;
  c->storage_prev = s->storage_tail;
  if (s->storage_tail) s->storage_tail->storage_next = c;
  else s->storage_head = c;
  s->storage_tail = c;
  ++s->storage_wait_count;
}
static inline client *storage_wait_pop(connections *s) {
  client *c = s->storage_head;
  if (c) {
    storage_wait_remove(s, c);
    queue_remove(c);
  }
  return c;
}
/* Run the oldest waiter immediately while capacity is free. No speculative
 * wake-all or reservations: it prepares its I/O before new work can take a slot. */
static inline client *connection_next(connections *s, const lb_store *store) {
  if (s->storage_head && store->pending_count < store->pending_limit)
    return storage_wait_pop(s);
  return queue_pop(&s->ready);
}
static inline void timer_place(connections *s, size_t i, client *c) {
  s->timers[i] = c;
  c->timer_index = i;
}
static inline void timer_fix(connections *s, size_t i) {
  client *c = s->timers[i];
  while (i && s->timers[(i - 1) / 2]->deadline > c->deadline) {
    timer_place(s, i, s->timers[(i - 1) / 2]);
    i = (i - 1) / 2;
  }
  while (i < s->timer_count / 2) {
    size_t child = i * 2 + 1;
    if (child + 1 < s->timer_count &&
        s->timers[child + 1]->deadline < s->timers[child]->deadline) ++child;
    if (s->timers[child]->deadline >= c->deadline) break;
    timer_place(s, i, s->timers[child]);
    i = child;
  }
  timer_place(s, i, c);
}
static inline void timer_remove(connections *s, client *c) {
  size_t i = c->timer_index;
  if (i == SIZE_MAX) return;
  --s->timer_count;
  if (i != s->timer_count) {
    timer_place(s, i, s->timers[s->timer_count]);
    timer_fix(s, i);
  }
  c->timer_index = SIZE_MAX;
}
static inline void timer_touch(connections *s, client *c, uint64_t deadline) {
  c->deadline = deadline;
  if (c->timer_index == SIZE_MAX) {
    /* Capacity is reserved before admitting the connection. */
    assert(s->timer_count < s->timer_capacity);
    timer_place(s, s->timer_count++, c);
  }
  timer_fix(s, c->timer_index);
}
static inline size_t connection_memory(const connections *s);
static inline void buffers_trim(buffer_pool *p);
static inline client *connection_take(connections *s) {
  client *c;
  size_t needed = s->free ? 0 : sizeof(client_slab);
  if (s->count == s->timer_capacity) {
    size_t capacity = s->timer_capacity ? s->timer_capacity : 8;
    if (s->timer_capacity && capacity > SIZE_MAX / 2) return NULL;
    if (s->timer_capacity) capacity *= 2;
    if (capacity > SIZE_MAX / sizeof(client *)) return NULL;
    needed += (capacity - s->timer_capacity) * sizeof(client *);
  }
  if (connection_memory(s) > s->memory_budget ||
      needed > s->memory_budget - connection_memory(s)) buffers_trim(&s->buffers);
  if (connection_memory(s) > s->memory_budget ||
      needed > s->memory_budget - connection_memory(s)) return NULL;
  if (!ct_array_reserve(&s->timers, &s->timer_capacity, s->count + 1,
                        sizeof(*s->timers))) return NULL;
  if (!s->free) {
    client_slab *slab = malloc(sizeof(*slab));
    if (!slab) return NULL;
    slab->next = s->slabs;
    s->slabs = slab;
    ++s->slab_count;
    for (size_t i = 0; i < LB_CLIENT_SLAB; ++i) {
      client_init(&slab->slots[i]);
      slab->slots[i].free_next = s->free;
      s->free = &slab->slots[i];
    }
  }
  c = s->free;
  s->free = c->free_next;
  c->free_next = NULL;
  c->live_next = s->live;
  if (s->live) s->live->live_prev = c;
  s->live = c;
  ++s->count;
  return c;
}
static inline void connection_release(connections *s, client *c) {
  assert(!c->receiving && !c->sending && !c->file_pending &&
         !c->cancelling_receive && !c->cancelling_send && !c->dirty_head &&
         !c->output_update && !c->delivery_bytes);
  queue_remove(c);
  storage_wait_remove(s, c);
  timer_remove(s, c);
  if (c->live_prev) c->live_prev->live_next = c->live_next;
  else s->live = c->live_next;
  if (c->live_next) c->live_next->live_prev = c->live_prev;
  s->client_bytes -= c->accounted;
  --s->count;
  client_init(c);
  c->free_next = s->free;
  s->free = c;
}
static inline size_t buffer_class_size(size_t i) {
  return i == LB_BUFFER_CLASSES - 1 ? LB_MAX_FRAME + 4 : (size_t)64 << i;
}
static inline size_t buffer_class(size_t size) {
  size_t i = 0;
  while (i + 1 < LB_BUFFER_CLASSES && buffer_class_size(i) < size) ++i;
  return i;
}
static inline void buffer_return(buffer_pool *p, lb_buffer *b) {
  if (b->data) {
    size_t i = buffer_class(b->capacity);
    assert(b->capacity == buffer_class_size(i));
    if (p->cached + b->capacity <= 8 * 1024 * 1024) {
      buffer_link *node = (buffer_link *)b->data;
      node->next = p->bins[i];
      p->bins[i] = node;
      p->cached += b->capacity;
    } else {
      p->allocated -= b->capacity;
      free(b->data);
    }
  }
  memset(b, 0, sizeof(*b));
}
static inline void buffers_trim(buffer_pool *p) {
  for (size_t i = 0; i < LB_BUFFER_CLASSES; ++i) {
    buffer_link *node;
    while ((node = p->bins[i])) {
      p->bins[i] = node->next;
      p->allocated -= buffer_class_size(i);
      free(node);
    }
  }
  p->cached = 0;
}
static inline size_t connection_memory(const connections *s) {
  return s->slab_count * sizeof(client_slab) + s->timer_capacity * sizeof(client *) +
      s->client_bytes + s->buffers.allocated + s->subscription_count * sizeof(subscription) +
      s->watch_count * sizeof(watch) + s->watches.capacity * sizeof(ct_pair) +
      s->watches.buckets * sizeof(uint32_t) + s->update_bytes +
      s->delivery_count * sizeof(delivery);
}
static inline bool connection_budget(connections *s) {
  if (connection_memory(s) <= s->memory_budget) return true;
  buffers_trim(&s->buffers);
  return connection_memory(s) <= s->memory_budget;
}
static inline bool buffer_borrow(connections *s, lb_buffer *b, size_t size) {
  size_t i = buffer_class(size), capacity = buffer_class_size(i);
  buffer_pool *p = &s->buffers;
  assert(!b->data);
  if (size > LB_MAX_FRAME + 4) return false;
  if (!connection_budget(s)) return false;
  if (p->bins[i]) {
    buffer_link *node = p->bins[i];
    p->bins[i] = node->next;
    p->cached -= capacity;
    b->data = (uint8_t *)node;
  } else {
    if (connection_memory(s) > s->memory_budget ||
        capacity > s->memory_budget - connection_memory(s)) buffers_trim(p);
    if (connection_memory(s) > s->memory_budget ||
        capacity > s->memory_budget - connection_memory(s)) return false;
    b->data = malloc(capacity);
    if (!b->data) return false;
    p->allocated += capacity;
  }
  b->capacity = capacity;
  return true;
}
static inline void connection_account(connections *s, client *c) {
  size_t bytes = c->channel_capacity * sizeof(channel) +
      c->channel_index.capacity * sizeof(ct_pair) + c->channel_index.buckets * sizeof(uint32_t) +
      c->task.payload.capacity + c->task.reply.capacity + c->task.records.capacity +
      c->task.read_buffer.capacity + c->task.count * sizeof(lb_change) +
      c->task.extra_count * sizeof(lb_store_request) + c->file_capacity * sizeof(completion);
  s->client_bytes -= c->accounted;
  s->client_bytes += bytes;
  c->accounted = bytes;
}
static inline bool delivery_reserve(connections *s, size_t bytes) {
  if (connection_memory(s) > s->memory_budget ||
      bytes > s->memory_budget - connection_memory(s)) buffers_trim(&s->buffers);
  return connection_memory(s) <= s->memory_budget &&
      bytes <= s->memory_budget - connection_memory(s);
}
static inline void update_release(connections *s, shared_update *update) {
  assert(update && update->refs);
  if (!--update->refs) {
    s->update_bytes -= sizeof(*update) + update->size;
    free(update);
  }
}
/* Transfer a queue's reference to the caller, including its logical byte cost. */
static inline shared_update *delivery_pop(connections *s, subscription *sub) {
  delivery *d = sub->head;
  shared_update *update;
  if (!d) return NULL;
  update = d->update;
  sub->head = d->next;
  if (!sub->head) sub->tail = NULL;
  sub->owner->delivery_bytes -= sizeof(*d);
  --s->delivery_count;
  free(d);
  return update;
}
static inline void delivery_release(connections *s, client *c, shared_update *update) {
  c->delivery_bytes -= sizeof(c->output_header) + update->size;
  update_release(s, update);
}
static inline void deliveries_clear(connections *s, subscription *sub) {
  shared_update *update;
  while ((update = delivery_pop(s, sub))) delivery_release(s, sub->owner, update);
}
static inline void output_update_release(connections *s, client *c) {
  if (!c->output_update) return;
  delivery_release(s, c, c->output_update);
  c->output_update = NULL;
  c->sent = 0;
}
static inline void delivery_integer(uint8_t *p, uint64_t value, unsigned bytes) {
  for (unsigned i = 0; i < bytes; ++i)
    p[bytes - i - 1] = (uint8_t)(value >> (8 * i));
}
static inline void delivery_start(connections *s, subscription *sub, channel *ch) {
  client *c = sub->owner;
  assert(!client_output_size(c) && sub->head);
  c->output_update = delivery_pop(s, sub);
  /* Connection-owned frame prefix; shared body is never copied per peer. */
  delivery_integer(c->output_header, c->output_update->size + 8, 4);
  delivery_integer(c->output_header + 4, 0, 4);
  delivery_integer(c->output_header + 8, ch->id, 4);
  c->sent = 0;
  ch->known = true;
  ch->announced = c->output_update->after;
}
static inline void subscription_clean(subscription *sub) {
  client *c = sub->owner;
  if (!sub->dirty) return;
  if (sub->dirty_prev) sub->dirty_prev->dirty_next = sub->dirty_next;
  else c->dirty_head = sub->dirty_next;
  if (sub->dirty_next) sub->dirty_next->dirty_prev = sub->dirty_prev;
  else c->dirty_tail = sub->dirty_prev;
  sub->dirty_prev = sub->dirty_next = NULL;
  sub->dirty = false;
}
static inline void subscription_dirty(connections *s, subscription *sub) {
  client *c = sub->owner;
  if (c->closing) return;
  if (!sub->dirty) {
    sub->dirty = true;
    sub->dirty_prev = c->dirty_tail;
    if (c->dirty_tail) c->dirty_tail->dirty_next = sub;
    else c->dirty_head = sub;
    c->dirty_tail = sub;
  }
  queue_push(&s->ready, c);
}
static inline watch *watch_find(connections *s, lb_namespace ns) {
  ct_pair *p = ct_map_get(&s->watches, ns.low64);
  watch *w = p ? (watch *)p->value : NULL;
  while (w && !lb_namespace_equal(w->ns, ns)) w = w->collision_next;
  return w;
}
static inline bool subscription_add(connections *s, client *c, channel *ch) {
  lb_namespace ns = lb_name(ch->name);
  watch *w = watch_find(s, ns);
  subscription *sub;
  if (ch->subscription) {
    subscription_dirty(s, ch->subscription);
    return true;
  }
  sub = calloc(1, sizeof(*sub));
  if (!sub) return false;
  if (!w) {
    ct_pair *p = ct_map_get(&s->watches, ns.low64);
    w = calloc(1, sizeof(*w));
    if (!w) { free(sub); return false; }
    w->ns = ns;
    w->collision_next = p ? (watch *)p->value : NULL;
    if (!ct_map_put(&s->watches, ns.low64, (uintptr_t)w)) {
      free(w); free(sub); return false;
    }
    ++s->watch_count;
  }
  sub->owner = c;
  sub->channel_id = ch->id;
  sub->watch = w;
  sub->next = w->head;
  if (w->head) w->head->prev = sub;
  w->head = sub;
  ch->subscription = sub;
  ++s->subscription_count;
  subscription_dirty(s, sub);
  return true;
}
static inline void subscription_remove(connections *s, subscription *sub) {
  watch *w;
  if (!sub) return;
  w = sub->watch;
  subscription_clean(sub);
  deliveries_clear(s, sub);
  if (sub->prev) sub->prev->next = sub->next;
  else w->head = sub->next;
  if (sub->next) sub->next->prev = sub->prev;
  free(sub);
  --s->subscription_count;
  if (!w->head) {
    ct_pair *p = ct_map_get(&s->watches, w->ns.low64);
    watch *head = (watch *)p->value;
    if (head == w) p->value = (uintptr_t)w->collision_next;
    else {
      while (head->collision_next != w) head = head->collision_next;
      head->collision_next = w->collision_next;
    }
    if (!p->value) ct_map_erase(&s->watches, w->ns.low64);
    free(w);
    --s->watch_count;
  }
}
static inline void connections_published(void *context, lb_namespace ns, uint64_t before,
                                         uint64_t after, uint64_t epoch, uint64_t fingerprint,
                                         lb_bytes records) {
  connections *s = context;
  watch *w = watch_find(s, ns);
  shared_update *update = NULL;
  bool unavailable = records.size > LB_MAX_FRAME - 33;
  if (!w) return; /* No history retained for absent/future subscribers. */
  for (subscription *sub = w->head; sub; sub = sub->next) {
    client *c = sub->owner;
    ct_pair *found;
    channel *ch;
    size_t cost = sizeof(delivery) + sizeof(c->output_header) + 25 + records.size;
    delivery *d;
    if (c->closing) continue;
    found = ct_map_get(&c->channel_index, sub->channel_id);
    assert(found);
    ch = &c->channels[found->value];
    if (!ch->optimistic || !ch->known || sub->resync ||
        (sub->tail ? sub->tail->update->after : ch->announced) != before ||
        cost > LB_DELIVERY_LIMIT || c->delivery_bytes > LB_DELIVERY_LIMIT - cost)
      goto fallback;
    if (!update && !unavailable) {
      size_t bytes = sizeof(*update) + 25 + records.size;
      if (delivery_reserve(s, bytes + sizeof(*d))) update = malloc(bytes);
      if (update) {
        update->refs = 1; /* Publisher retains a reference through fanout. */
        update->size = 25 + records.size;
        update->before = before;
        update->after = after;
        update->data[0] = 5;
        delivery_integer(update->data + 1, epoch, 8);
        delivery_integer(update->data + 9, after, 8);
        delivery_integer(update->data + 17, fingerprint, 8);
        if (records.size) memcpy(update->data + 25, records.data, records.size);
        s->update_bytes += bytes;
      } else unavailable = true;
    }
    if (!update || !delivery_reserve(s, sizeof(*d))) goto fallback;
    d = malloc(sizeof(*d));
    if (!d) goto fallback;
    d->next = NULL;
    d->update = update;
    ++update->refs;
    ++s->delivery_count;
    c->delivery_bytes += cost;
    if (sub->tail) sub->tail->next = d;
    else sub->head = d;
    sub->tail = d;
    subscription_dirty(s, sub);
    continue;
fallback:
    /* Only this subscription loses its chain. Already submitted socket bytes
     * remain pinned until completion; send a fresh opening after they drain. */
    deliveries_clear(s, sub);
    sub->resync = true;
    subscription_dirty(s, sub);
  }
  if (update) update_release(s, update);
}
static inline void connections_changed(void *context, lb_namespace ns,
                                       const lb_publication *publication) {
  if (publication) {
    connections_published(context, ns, publication->before, publication->after,
                           publication->epoch, publication->fingerprint, publication->records);
    return;
  }
  connections *s = context;
  watch *w = watch_find(s, ns);
  if (w) for (subscription *sub = w->head; sub; sub = sub->next)
    subscription_dirty(s, sub);
}
static inline void connections_destroy(connections *s) {
  assert(!s->count && !s->watch_count && !s->subscription_count);
  assert(!s->storage_wait_count && !s->storage_head && !s->storage_tail);
  assert(!s->update_bytes && !s->delivery_count);
  buffers_trim(&s->buffers);
  assert(!s->buffers.allocated);
  while (s->slabs) {
    client_slab *next = s->slabs->next;
    free(s->slabs);
    s->slabs = next;
  }
  free(s->timers);
  ct_map_free(&s->watches);
  memset(s, 0, sizeof(*s));
}
#endif
