#ifndef LB_REQUEST_LIFETIME_H
#define LB_REQUEST_LIFETIME_H
#include "models.h"
#include "namespaces.h"
#include "storage.h"
#include "codec.h"
/* Stable kernel-visible slots, independent of the operation using them. */
typedef struct lb_pending_io {
  lb_store_request first, *extra;
  size_t extra_count;
} lb_pending_io;
typedef enum { lb_task_new, lb_task_active, lb_task_done } lb_task_phase;
typedef enum { lb_abort_none, lb_abort_restart, lb_abort_reject } lb_task_abort;
typedef struct lb_task {
  unsigned operation;
  lb_namespace ns;
  uint64_t epoch, version;
  lb_buffer payload, reply;
  lb_pending_io pending;
  /* The operation defines this allocation's type and releases its children.
   * Lifetime code frees the allocation only after operation cleanup and I/O drain. */
  void *state;
  size_t state_bytes; /* allocation plus children, maintained by the operation */
  lb_task_phase phase;
  lb_task_abort abort_status;
  bool waiting_capacity;
  lb_reply_kind reply_kind;
  /* Outward effects consumed by the caller before borrowed payload bytes die. */
  bool changed, publish;
  lb_publication publication;
} lb_task;
static inline lb_store_request *lb_pending_slot(lb_pending_io *io, size_t slot) {
  return slot ? &io->extra[slot - 1] : &io->first;
}
static inline lb_store_request *lb_task_io(lb_task *t, size_t slot) {
  return lb_pending_slot(&t->pending, slot);
}
static inline bool lb_pending_reserve(lb_pending_io *io, size_t slots) {
  assert(!io->extra && !io->extra_count);
  if (slots <= 1) return true;
  io->extra = (lb_store_request *)calloc(slots - 1, sizeof(*io->extra));
  if (!io->extra) return false;
  io->extra_count = slots - 1;
  return true;
}
static inline bool lb_task_io_ready(lb_task *t) {
  for (size_t i = 0; i <= t->pending.extra_count; ++i) {
    lb_store_request *r = lb_task_io(t, i);
    if (r->owner && r->state == lb_phase_ready) return true;
  }
  return false;
}
static inline bool lb_task_drain(lb_task *t) {
  bool drained = true;
  for (size_t i = 0; i <= t->pending.extra_count; ++i) {
    lb_store_request *r = lb_task_io(t, i);
    if (!r->owner) continue;
    if (r->state == lb_phase_submitted) drained = false;
    else lb_store_cancel(r);
  }
  return drained;
}
static inline bool lb_task_stop(lb_task *t, lb_task_abort reason) {
  t->waiting_capacity = false;
  t->abort_status = reason;
  return lb_task_drain(t);
}
static inline void lb_task_dispose(lb_task *t) {
  if (!lb_task_drain(t)) abort();
  assert(!t->changed);
  lb_buffer_free(&t->payload);
  lb_buffer_free(&t->reply);
  free(t->pending.extra);
  free(t->state);
  memset(t, 0, sizeof(*t));
}
static inline bool lb_task_prepare(lb_task *t, unsigned operation, lb_namespace ns,
                                   uint64_t epoch, uint64_t revision, lb_bytes payload) {
  memset(t, 0, sizeof(*t));
  t->operation = operation;
  t->ns = ns;
  t->epoch = epoch;
  t->version = revision;
  return lb_append(&t->payload, payload.data, payload.size);
}
static inline bool lb_task_allocate(lb_task *t, size_t bytes) {
  assert(!t->state);
  t->state = calloc(1, bytes);
  if (!t->state) return false;
  t->state_bytes = bytes;
  return true;
}
static inline void lb_task_ready(lb_task *t, lb_buffer *reply) {
  memset(t, 0, sizeof(*t));
  t->reply = *reply;
  memset(reply, 0, sizeof(*reply));
  t->phase = lb_task_done;
}
#endif
