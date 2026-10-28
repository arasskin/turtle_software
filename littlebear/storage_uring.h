#ifndef LB_STORAGE_URING_H
#define LB_STORAGE_URING_H
#include "storage.h"
#include <liburing.h>
static inline bool lb_uring_enqueue(struct io_uring *ring, lb_store_request *request, void *tag) {
  struct io_uring_sqe *sqe;
  lb_store_io_operation op = CT_ZERO;
  if (!request->owner || request->state != lb_phase_ready)
    return false;
  sqe = io_uring_get_sqe(ring);
  if (!sqe)
    return false;
  lb_store_start_io(request, &op);
  if (op.writing)
    io_uring_prep_write(sqe, op.fd, op.buffer, op.length, op.offset);
  else
    io_uring_prep_read(sqe, op.fd, op.buffer, op.length, op.offset);
  io_uring_sqe_set_data(sqe, tag ? tag : request);
  return true;
}
static inline lb_store_result lb_uring_complete(const struct io_uring_cqe *cqe) {
  return lb_store_complete_io((lb_store_request *)io_uring_cqe_get_data(cqe), cqe->res);
}
#endif
