#ifndef LB_CLIENT_CONNECTIONS_H
#define LB_CLIENT_CONNECTIONS_H
#include "../containers/map.h"
#include "models.h"
#include "request_lifetime.h"
#include "subscriber_delivery.h"
#include <sys/socket.h>
#include <sys/uio.h>
typedef struct client client;
typedef enum {
  op_accept, op_receive, op_send, op_file, op_cancel_receive, op_cancel_send,
  op_cancel_accept
} operation;
typedef struct completion {
  operation kind;
  client *owner;
  lb_store_request *io;
} completion;
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
  struct lb_websocket *websocket;
  size_t websocket_bytes;
  uint8_t websocket_prefix[10];
  size_t websocket_prefix_size;
  struct iovec output_vectors[3];
  struct msghdr output_message;
  lb_task task;
  lb_peer peer;
  uint32_t request_id, channel_id;
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
typedef struct connections {
  client_slab *slabs;
  client *free, *live;
  size_t count, slab_count, client_bytes;
  client_queue ready, submission_wait;
  client *storage_head, *storage_tail;
  size_t storage_wait_count;
  client **timers;
  size_t timer_count, timer_capacity;
  buffer_pool *buffers; /* borrowed from server lifetime */
  size_t memory_budget;
  lb_subscribers *subscribers; /* borrowed from server lifetime */
} connections;

static inline bool connection_delivery_reserve(void *context, size_t bytes);
static inline void connection_delivery_wake(void *context, lb_peer *peer);
static inline size_t client_output_size(const client *c) {
  return c->websocket_prefix_size +
      (c->peer.output_update ? sizeof(c->peer.output_header) + c->peer.output_update->size : c->output.size);
}

static inline void client_init(client *c) {
  memset(c, 0, sizeof(*c));
  c->fd = -1;
  c->peer.context = c;
  c->peer.active = c->timer_index = SIZE_MAX;
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
      needed > s->memory_budget - connection_memory(s)) buffers_trim(s->buffers);
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
         !c->cancelling_receive && !c->cancelling_send && !c->peer.dirty_head &&
         !c->peer.output_update && !c->peer.delivery_bytes);
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
      s->client_bytes + s->buffers->allocated + subscriber_memory(s->subscribers);
}
static inline bool connection_budget(connections *s) {
  if (connection_memory(s) <= s->memory_budget) return true;
  buffers_trim(s->buffers);
  return connection_memory(s) <= s->memory_budget;
}
static inline bool buffer_borrow(connections *s, lb_buffer *b, size_t size) {
  size_t i = buffer_class(size), capacity = buffer_class_size(i);
  buffer_pool *p = s->buffers;
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
  size_t bytes = c->websocket_bytes + c->peer.channel_capacity * sizeof(channel) +
      c->peer.channel_index.capacity * sizeof(ct_pair) + c->peer.channel_index.buckets * sizeof(uint32_t) +
      c->task.payload.capacity + c->task.reply.capacity + c->task.state_bytes +
      c->task.pending.extra_count * sizeof(lb_store_request) + c->file_capacity * sizeof(completion);
  s->client_bytes -= c->accounted;
  s->client_bytes += bytes;
  c->accounted = bytes;
}
static inline void connections_destroy(connections *s) {
  assert(!s->count && !s->subscribers->watch_count && !s->subscribers->subscription_count);
  assert(!s->storage_wait_count && !s->storage_head && !s->storage_tail);
  assert(!s->subscribers->update_bytes && !s->subscribers->delivery_count);
  while (s->slabs) {
    client_slab *next = s->slabs->next;
    free(s->slabs);
    s->slabs = next;
  }
  free(s->timers);
  memset(s, 0, sizeof(*s));
}
static inline bool connection_delivery_reserve(void *context, size_t bytes) {
  connections *s = context;
  if (connection_memory(s) > s->memory_budget ||
      bytes > s->memory_budget - connection_memory(s)) buffers_trim(s->buffers);
  return connection_memory(s) <= s->memory_budget && bytes <= s->memory_budget - connection_memory(s);
}
static inline void connection_delivery_wake(void *context, lb_peer *peer) {
  connections *s = context;
  client *c = peer->context;
  if (!c->closing) queue_push(&s->ready, c);
}
#endif
