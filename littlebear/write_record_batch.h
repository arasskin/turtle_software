#ifndef LB_WRITE_RECORD_BATCH_H
#define LB_WRITE_RECORD_BATCH_H
#include "request_lifetime.h"
#include "record_batch_codec.h"
#include "storage.h"
typedef enum {
  lb_batch_finished, lb_batch_pending, lb_batch_capacity, lb_batch_conflict, lb_batch_failed
} lb_batch_result;
static inline lb_batch_result lb_parallel_writes(lb_record_batch *batch, lb_pending_io *io,
                                                  lb_store *records, lb_namespace ns,
                                                  size_t active_writers, size_t budget) {
  size_t active = 0;
  /* More independent writers already supply I/O concurrency. Share the pool
   * instead of letting each one reserve a full window. Existing reservations
   * drain normally if the fair share shrinks while they are in flight. */
  size_t share = CT_MAX((size_t)1, records->pending_limit /
                                      CT_MAX((size_t)1, active_writers));
  share = CT_MIN(share, io->extra_count + 1);
  for (size_t i = 0; i <= io->extra_count; ++i) {
    lb_store_request *r = lb_pending_slot(io, i);
    if (r->owner) ++active;
    if (r->state == lb_phase_finished && r->outcome.code != lb_error_ok) {
      return lb_batch_failed;
    }
  }
  for (size_t i = 0; i <= io->extra_count; ++i) {
    lb_store_request *r = lb_pending_slot(io, i);
    if (r->owner) continue;
    r->state = lb_phase_idle;
    while (batch->position < batch->count && budget && active < share) {
      lb_change *c = &batch->changes[batch->position];
      lb_store_result rc;
      --budget;
      if (c->present)
        rc = lb_store_prepare_write(records, r, ns, c->id,
                                    c->value.data, c->value.size, lb_integer(c->value));
      else {
        rc = lb_store_erase_record(records, ns, c->id);
        if (rc.code == lb_error_record_missing) rc.code = lb_error_ok;
      }
      if (rc.code == lb_error_pending_full) {
        return lb_batch_capacity;
      }
      if (rc.code == lb_error_busy) {
        return lb_batch_conflict;
      }
      if (rc.code != lb_error_ok) {
        return lb_batch_failed;
      }
      ++batch->position;
      if (r->owner) { ++active; break; }
    }
  }
  if (batch->position != batch->count) return lb_batch_pending;
  for (size_t i = 0; i <= io->extra_count; ++i)
    if (lb_pending_slot(io, i)->owner) return lb_batch_pending;
  return lb_batch_finished;
}
/* Execute an already validated batch in an existing, operation-locked namespace.
 * The caller owns namespace creation, revision changes, drain and response policy. */
static inline lb_batch_result lb_write_record_batch(lb_record_batch *batch, lb_pending_io *io,
                                                    lb_store *records, lb_namespace ns,
                                                    size_t active_writers, size_t budget) {
  if (!io->extra_count) {
    if (io->first.state == lb_phase_finished) {
      if (io->first.outcome.code != lb_error_ok) { return lb_batch_failed; }
      ++batch->position;
      io->first.state = lb_phase_idle;
    } else if (io->first.owner) return lb_batch_pending;
  }
  if (io->extra_count) return lb_parallel_writes(batch, io, records, ns, active_writers, budget);
  while (batch->position < batch->count) {
    lb_change *change = &batch->changes[batch->position];
    lb_store_result rc;
    if (!budget--) return lb_batch_pending;
    if (change->present)
      rc = lb_store_prepare_write(records, &io->first, ns, change->id,
                                  change->value.data, change->value.size, lb_integer(change->value));
    else {
      rc = lb_store_erase_record(records, ns, change->id);
      if (rc.code == lb_error_record_missing) rc.code = lb_error_ok;
    }
    if (rc.code == lb_error_pending_full) { return lb_batch_capacity; }
    if (rc.code == lb_error_busy) { return lb_batch_conflict; }
    if (rc.code != lb_error_ok) { return lb_batch_failed; }
    if (io->first.owner) return lb_batch_pending;
    ++batch->position;
  }
  return lb_batch_finished;
}
#endif
