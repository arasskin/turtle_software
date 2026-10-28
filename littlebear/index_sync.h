#ifndef LB_INDEX_SYNC_H
#define LB_INDEX_SYNC_H
#include "reconcile.h"
#include "index.h"
static inline ct_range lb_index_bounds(lb_recon_range p) {
  ct_range r = {p.low, p.high, p.high != 0};
  return r;
}
static inline bool lb_index_summarize_cb(void *ctx, lb_recon_range p, lb_recon_summary *out) {
  ct_summary s = lb_index_summary((lb_index *)ctx, lb_index_bounds(p));
  out->count = s.count;
  out->fingerprint = s.hash;
  return true;
}
static inline bool lb_index_select_cb(void *ctx, uint64_t rank, uint64_t *out) {
  ct_record r;
  if (!lb_index_select((lb_index *)ctx, rank, &r))
    return false;
  *out = r.key;
  return true;
}
static inline size_t lb_index_list_cb(void *ctx, lb_recon_range p, lb_recon_item *out,
                                      size_t capacity) {
  lb_index *h = (lb_index *)ctx;
  ct_range before = {0, p.low, true};
  uint64_t first = lb_index_summary(h, before).count,
           count = lb_index_summary(h, lb_index_bounds(p)).count;
  size_t i, n = (size_t)CT_MIN((uint64_t)capacity, count);
  for (i = 0; i < n; ++i) {
    ct_record r;
    if (!lb_index_select(h, first + i, &r))
      return SIZE_MAX;
    out[i].id = r.key;
    out[i].fingerprint = r.hash;
  }
  return n;
}
static inline lb_recon_source lb_index_source(lb_index *h) {
  lb_recon_source s = {h, lb_index_summarize_cb, lb_index_select_cb, lb_index_list_cb};
  return s;
}
#endif
