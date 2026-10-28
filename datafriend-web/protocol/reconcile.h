#ifndef LB_RECONCILE_H
#define LB_RECONCILE_H
#include "base.h"
static const uint8_t lb_recon_MAGIC[] = {'L', 'B', 'R', 3};
enum {
  lb_recon_ID_BYTES = 8,
  lb_recon_FINGERPRINT_BYTES = 8,
  lb_recon_LEAF_IDS = 32,
  lb_recon_FANOUT = 16,
  lb_recon_MAX_SPLIT_BYTES = lb_recon_FANOUT * (2 + 16 + lb_recon_FINGERPRINT_BYTES),
  lb_recon_MAX_LIST_BYTES = 2 + 16 + 1 +
      lb_recon_LEAF_IDS * (lb_recon_ID_BYTES + lb_recon_FINGERPRINT_BYTES),
  lb_recon_MIN_OUTPUT_CAPACITY = sizeof(lb_recon_MAGIC) +
      (lb_recon_MAX_SPLIT_BYTES > lb_recon_MAX_LIST_BYTES ? lb_recon_MAX_SPLIT_BYTES
                                                          : lb_recon_MAX_LIST_BYTES)
};

// [low, high). high=0 means infinity, so {} covers every ID, including UINT64_MAX.
typedef struct lb_recon_range {
  uint64_t low, high;
} lb_recon_range;
static inline bool lb_recon_valid(lb_recon_range r) {
  return r.high == 0 || r.low < r.high;
}
static inline bool lb_recon_contains(lb_recon_range r, uint64_t id) {
  return id >= r.low && (r.high == 0 || id < r.high);
}
static inline bool lb_recon_upper_bound(lb_recon_range r, uint64_t *out) {
  if (r.high == 0)
    return false;
  *out = r.high;
  return true;
}
typedef struct lb_recon_summary {
  uint64_t count, fingerprint;
} lb_recon_summary;
typedef struct lb_recon_item {
  uint64_t id, fingerprint;
} lb_recon_item;
// All callbacks read one stable snapshot. summarize uses cached count/XOR
// accumulators; select returns the ID at a global zero-based rank. list writes
// the first min(size,capacity) items sorted by unique ID, or SIZE_MAX on error.
// Range XORs combine stored fingerprints, never IDs. The caller chooses both.
typedef struct lb_recon_source {
  void *context;
  bool (*summarize)(void *, lb_recon_range, lb_recon_summary *);
  bool (*select)(void *, uint64_t, uint64_t *);
  size_t (*list)(void *, lb_recon_range, lb_recon_item *, size_t);
} lb_recon_source;
typedef struct lb_recon_bytes {
  const uint8_t *data;
  size_t size;
} lb_recon_bytes;
typedef struct lb_recon_output {
  uint8_t *data;
  size_t capacity, size;
  uint64_t previous_high;
} lb_recon_output;
// A shared ID with unequal fingerprints appears in BOTH have and need. The
// driver applies namespace authority to choose the version; this is not deletion.
typedef struct lb_recon_differences {
  uint64_t *have;
  size_t have_capacity, have_count;
  uint64_t *need;
  size_t need_capacity, need_count;
} lb_recon_differences;
// One lb_recon_cursor per incoming packet. Keep packet lb_recon_bytes alive while resuming it.
typedef struct lb_recon_cursor {
  size_t offset;
  lb_recon_range working;
  bool traversing;
  uint64_t previous_high;
} lb_recon_cursor;
typedef enum { lb_recon_role_initiator, lb_recon_role_responder } lb_recon_role;
typedef enum {
  lb_recon_status_complete,
  lb_recon_status_more,
  lb_recon_status_output_full,
  lb_recon_status_differences_full,
  lb_recon_status_invalid_input,
  lb_recon_status_source_error,
  lb_recon_status_invalid_argument
} lb_recon_status;
typedef enum {
  lb_recon_kind_summary = 0,
  lb_recon_kind_offer = 1,
  lb_recon_kind_answer = 2
} lb_recon_kind;
typedef struct lb_recon_workspace {
  lb_recon_item own[lb_recon_LEAF_IDS + 1]; // Also holds the 16 cut boundaries while refining.
  lb_recon_item remote[lb_recon_LEAF_IDS];
  uint64_t have[lb_recon_LEAF_IDS], need[lb_recon_LEAF_IDS];
  uint64_t summaries[lb_recon_FANOUT];
} lb_recon_workspace;
typedef struct lb_recon_frame {
  lb_recon_range part;
  lb_recon_kind type;
  uint64_t fingerprint;
  size_t count;
} lb_recon_frame;

static inline size_t lb_recon_integer_bytes(uint64_t value) {
  size_t width = 0;
  while (value) {
    ++width;
    value >>= 8;
  }
  return width;
}
// Omit whole ID lb_recon_bytes shared by every integer in this lb_recon_range.
static inline size_t lb_recon_suffix_bytes(lb_recon_range r) {
  return lb_recon_integer_bytes(r.low ^ (r.high ? r.high - 1 : UINT64_MAX));
}
static inline size_t lb_recon_frame_size(lb_recon_range r, lb_recon_kind type, size_t count) {
  return 2 + lb_recon_integer_bytes(r.low) + lb_recon_integer_bytes(r.high) +
         (type == lb_recon_kind_summary
              ? (size_t)lb_recon_FINGERPRINT_BYTES
              : 1 + count * (lb_recon_suffix_bytes(r) + lb_recon_FINGERPRINT_BYTES));
}
static inline void lb_recon_write_integer(lb_recon_output *out, uint64_t value, size_t width) {
  for (size_t i = width; i != 0; --i)
    out->data[out->size++] = (uint8_t)(value >> (8 * (i - 1)));
}
static inline void lb_recon_write_frame(lb_recon_output *out, lb_recon_range r, lb_recon_kind type,
                                        const lb_recon_item *items, size_t count,
                                        uint64_t fingerprint) {
  // A first lb_recon_frame is always self-contained, so streamed chunks can be joined.
  const bool adjacent = out->previous_high != 0 && r.low == out->previous_high;
  out->data[out->size++] = (uint8_t)(type);
  out->data[out->size++] = (uint8_t)(((adjacent ? 9 : lb_recon_integer_bytes(r.low)) << 4) |
                                     lb_recon_integer_bytes(r.high));
  if (!adjacent)
    lb_recon_write_integer(out, r.low, lb_recon_integer_bytes(r.low));
  lb_recon_write_integer(out, r.high, lb_recon_integer_bytes(r.high));
  out->previous_high = r.high;
  if (type == lb_recon_kind_summary)
    lb_recon_write_integer(out, fingerprint, lb_recon_FINGERPRINT_BYTES);
  else {
    out->data[out->size++] = (uint8_t)(count);
    for (size_t i = 0; i < count; ++i) {
      lb_recon_write_integer(out, items[i].id, lb_recon_suffix_bytes(r));
      lb_recon_write_integer(out, items[i].fingerprint, lb_recon_FINGERPRINT_BYTES);
    }
  }
}
static inline bool lb_recon_take(lb_recon_bytes packet, size_t *position, size_t width,
                                 uint64_t *value) {
  if (*position > packet.size || width > packet.size - *position)
    return false;
  *value = 0;
  for (size_t i = 0; i < width; ++i)
    *value = (*value << 8) | packet.data[(*position)++];
  return true;
}
static inline bool lb_recon_read_frame(lb_recon_bytes packet, size_t *position, lb_recon_frame *f,
                                       lb_recon_workspace *w, uint64_t previous_high) {
  uint64_t type, widths;
  if (!lb_recon_take(packet, position, 1, &type) || type > (uint8_t)(lb_recon_kind_answer) ||
      !lb_recon_take(packet, position, 1, &widths))
    return false;
  f->type = (lb_recon_kind)(type);
  const size_t low_width = widths >> 4, high_width = widths & 15;
  if (low_width > 9 || high_width > 8 || (low_width == 9 && previous_high == 0))
    return false;
  f->part.low = previous_high;
  if ((low_width != 9 && (!lb_recon_take(packet, position, low_width, &f->part.low) ||
                          lb_recon_integer_bytes(f->part.low) != low_width)) ||
      !lb_recon_take(packet, position, high_width, &f->part.high) ||
      lb_recon_integer_bytes(f->part.high) != high_width || !lb_recon_valid(f->part))
    return false;
  if (f->type == lb_recon_kind_summary)
    return lb_recon_take(packet, position, lb_recon_FINGERPRINT_BYTES, &f->fingerprint);
  uint64_t value;
  if (!lb_recon_take(packet, position, 1, &value) || value > lb_recon_LEAF_IDS)
    return false;
  f->count = (size_t)(value);
  const size_t suffix = lb_recon_suffix_bytes(f->part);
  const uint64_t shared = suffix == 8 ? 0 : f->part.low & (UINT64_MAX << (suffix * 8));
  for (size_t i = 0; i < f->count; ++i) {
    if (!lb_recon_take(packet, position, suffix, &value))
      return false;
    w->remote[i].id = shared | value;
    if (!lb_recon_take(packet, position, lb_recon_FINGERPRINT_BYTES, &w->remote[i].fingerprint))
      return false;
    if (!lb_recon_contains(f->part, w->remote[i].id) ||
        (i != 0 && w->remote[i - 1].id >= w->remote[i].id))
      return false;
  }
  return true;
}
static inline bool lb_recon_read_own(lb_recon_source src, lb_recon_range r, lb_recon_workspace *w,
                                     size_t *count) {
  *count = src.list(src.context, r, w->own, lb_recon_LEAF_IDS + 1);
  if (*count > lb_recon_LEAF_IDS + 1)
    return false;
  for (size_t i = 0; i < *count; ++i)
    if (!lb_recon_contains(r, w->own[i].id) || (i != 0 && w->own[i - 1].id >= w->own[i].id))
      return false;
  return true;
}
static inline bool lb_recon_initialize_output(lb_recon_output *out) {
  out->size = 0;
  out->previous_high = 0;
  if (out->data == NULL || out->capacity < sizeof(lb_recon_MAGIC))
    return false;
  memcpy(out->data, lb_recon_MAGIC, sizeof(lb_recon_MAGIC));
  out->size = sizeof(lb_recon_MAGIC);
  return true;
}
static inline bool lb_recon_room(const lb_recon_output *out, size_t needed) {
  return needed <= out->capacity - out->size;
}

// Partition by accumulated record count. Each group has n/16 records, with one
// extra in the first n%16 groups. Rank selection supplies actual ID boundaries.
// Keep the parent's full extent, including gaps containing only the peer's IDs.
static inline lb_recon_status lb_recon_refine(lb_recon_source src, lb_recon_range parent,
                                              uint64_t count, lb_recon_workspace *w,
                                              lb_recon_output *out) {
  if (count <= lb_recon_LEAF_IDS)
    return lb_recon_status_source_error;
  lb_recon_summary before = CT_ZERO;
  if (parent.low && !src.summarize(src.context, (lb_recon_range){0, parent.low}, &before))
    return lb_recon_status_source_error;
  if (count > UINT64_MAX - before.count)
    return lb_recon_status_source_error;
  uint64_t rank = before.count, low = parent.low;
  const uint64_t per_group = count / lb_recon_FANOUT, extra = count % lb_recon_FANOUT;
  size_t needed = 0;
  for (size_t i = 0; i < lb_recon_FANOUT; ++i) {
    rank += per_group + (i < extra);
    uint64_t high = parent.high;
    if (i + 1 != lb_recon_FANOUT &&
        (!src.select(src.context, rank, &high) || high <= low || !lb_recon_contains(parent, high)))
      return lb_recon_status_source_error;
    w->own[i].id = high;
    needed += lb_recon_frame_size((lb_recon_range){low, high}, lb_recon_kind_summary, 0);
    low = high;
  }
  if (!lb_recon_room(out, needed))
    return lb_recon_status_output_full;
  low = parent.low;
  for (size_t i = 0; i < lb_recon_FANOUT; ++i) {
    lb_recon_summary child;
    if (!src.summarize(src.context, (lb_recon_range){low, w->own[i].id}, &child) ||
        child.count != per_group + (i < extra))
      return lb_recon_status_source_error;
    w->summaries[i] = child.fingerprint;
    low = w->own[i].id;
  }
  low = parent.low;
  for (size_t i = 0; i < lb_recon_FANOUT; ++i) {
    lb_recon_write_frame(out, (lb_recon_range){low, w->own[i].id}, lb_recon_kind_summary, NULL, 0,
                         w->summaries[i]);
    low = w->own[i].id;
  }
  return lb_recon_status_complete;
}
// Preserve the small opening: one whole-namespace XOR for larger sets. The
// count accumulator avoids probing IDs until explicit enumeration is needed.
static inline lb_recon_status lb_recon_begin(lb_recon_source src, lb_recon_workspace *w,
                                             lb_recon_output *out) {
  if (out)
    out->size = 0;
  if (!src.list || !src.summarize || !src.select || !w || !out)
    return lb_recon_status_invalid_argument;
  if (!lb_recon_initialize_output(out))
    return lb_recon_status_output_full;
  lb_recon_summary root;
  if (!src.summarize(src.context, (lb_recon_range)CT_ZERO, &root))
    return lb_recon_status_source_error;
  if (root.count <= lb_recon_LEAF_IDS) {
    if (!lb_recon_room(out, lb_recon_frame_size((lb_recon_range)CT_ZERO, lb_recon_kind_offer,
                                                (size_t)(root.count))))
      return lb_recon_status_output_full;
    size_t count;
    if (!lb_recon_read_own(src, (lb_recon_range)CT_ZERO, w, &count) || count != root.count)
      return lb_recon_status_source_error;
    lb_recon_write_frame(out, (lb_recon_range)CT_ZERO, lb_recon_kind_offer, w->own, count, 0);
  } else {
    if (!lb_recon_room(out, lb_recon_frame_size((lb_recon_range)CT_ZERO, lb_recon_kind_summary, 0)))
      return lb_recon_status_output_full;
    lb_recon_write_frame(out, (lb_recon_range)CT_ZERO, lb_recon_kind_summary, NULL, 0,
                         root.fingerprint);
  }
  return lb_recon_status_complete;
}

// Output/difference counts reset each call. *_full and more are resumable:
// drain results and retain the packet/lb_recon_cursor. Only the initiator gets have/need.
// A visit handles one lb_recon_summary (up to 16 child summaries) or at most 32 local IDs.
// Input must not overlap lb_recon_output, lb_recon_differences, or lb_recon_workspace. Finish a
// direction only once every input chunk is consumed and no response frames remain.
static inline lb_recon_status lb_recon_receive(lb_recon_source src, lb_recon_role side,
                                               lb_recon_bytes packet, lb_recon_cursor *at,
                                               lb_recon_workspace *w, lb_recon_output *out,
                                               lb_recon_differences *diff, size_t work_budget) {
  if (out)
    out->size = 0;
  if (diff)
    diff->have_count = diff->need_count = 0;
  if (!src.list || !src.summarize || !src.select || !w || !out || !diff || !at || !packet.data ||
      work_budget == 0 || (diff->have_capacity && !diff->have) ||
      (diff->need_capacity && !diff->need))
    return lb_recon_status_invalid_argument;
  if (packet.size < sizeof(lb_recon_MAGIC) ||
      memcmp(packet.data, lb_recon_MAGIC, sizeof(lb_recon_MAGIC)) != 0 ||
      (at->offset != 0 && at->offset < sizeof(lb_recon_MAGIC)) || at->offset > packet.size)
    return lb_recon_status_invalid_input;
  if (!lb_recon_initialize_output(out))
    return lb_recon_status_output_full;
  if (at->offset == 0)
    at->offset = sizeof(lb_recon_MAGIC);
  size_t visits = 0;
  while (at->offset != packet.size && visits < work_budget) {
    size_t end = at->offset;
    lb_recon_frame incoming = CT_ZERO;
    if (!lb_recon_read_frame(packet, &end, &incoming, w, at->previous_high))
      return lb_recon_status_invalid_input;
    if (incoming.type == lb_recon_kind_summary) {
      if (at->traversing)
        return lb_recon_status_invalid_input;
      ++visits;
      lb_recon_summary own;
      if (!src.summarize(src.context, incoming.part, &own))
        return lb_recon_status_source_error;
      if (incoming.fingerprint != own.fingerprint) {
        if (own.count > lb_recon_LEAF_IDS) {
          const lb_recon_status refined = lb_recon_refine(src, incoming.part, own.count, w, out);
          if (refined != lb_recon_status_complete)
            return refined;
        } else {
          if (!lb_recon_room(out, lb_recon_frame_size(incoming.part, lb_recon_kind_offer,
                                                      (size_t)(own.count))))
            return lb_recon_status_output_full;
          size_t count;
          if (!lb_recon_read_own(src, incoming.part, w, &count) || count != own.count)
            return lb_recon_status_source_error;
          lb_recon_write_frame(out, incoming.part, lb_recon_kind_offer, w->own, count, 0);
        }
      }
      at->offset = end;
      at->previous_high = incoming.part.high;
      continue;
    }
    if (side == lb_recon_role_responder && incoming.type == lb_recon_kind_answer)
      return lb_recon_status_invalid_input;
    if (!at->traversing) {
      at->working = incoming.part;
      at->traversing = true;
    }
    if (!lb_recon_valid(at->working) || at->working.high != incoming.part.high ||
        !lb_recon_contains(incoming.part, at->working.low))
      return lb_recon_status_invalid_input;
    while (visits < work_budget) {
      ++visits;
      size_t own_count;
      if (!lb_recon_read_own(src, at->working, w, &own_count))
        return lb_recon_status_source_error;
      lb_recon_range chunk = at->working;
      const bool remaining = own_count > lb_recon_LEAF_IDS;
      if (remaining) {
        chunk.high = w->own[lb_recon_LEAF_IDS].id;
        own_count = lb_recon_LEAF_IDS;
      }
      if (side == lb_recon_role_responder) {
        if (!lb_recon_room(out, lb_recon_frame_size(chunk, lb_recon_kind_answer, own_count)))
          return lb_recon_status_output_full;
        lb_recon_write_frame(out, chunk, lb_recon_kind_answer, w->own, own_count, 0);
      } else {
        size_t first = 0;
        while (first < incoming.count && w->remote[first].id < chunk.low)
          ++first;
        size_t count = 0;
        while (first + count < incoming.count &&
               lb_recon_contains(chunk, w->remote[first + count].id))
          ++count;
        const lb_recon_item *remote = w->remote + first;
        size_t i = 0, j = 0, have = 0, need = 0;
        while (i < own_count || j < count) {
          if (j == count || (i < own_count && w->own[i].id < remote[j].id))
            w->have[have++] = w->own[i++].id;
          else if (i == own_count || remote[j].id < w->own[i].id)
            w->need[need++] = remote[j++].id;
          else {
            if (w->own[i].fingerprint != remote[j].fingerprint) {
              w->have[have++] = w->own[i].id;
              w->need[need++] = remote[j].id;
            }
            ++i;
            ++j;
          }
        }
        if (have > diff->have_capacity - diff->have_count ||
            need > diff->need_capacity - diff->need_count)
          return lb_recon_status_differences_full;
        if (have)
          memcpy(diff->have + diff->have_count, w->have, have * sizeof(uint64_t));
        if (need)
          memcpy(diff->need + diff->need_count, w->need, need * sizeof(uint64_t));
        diff->have_count += have;
        diff->need_count += need;
      }
      if (!remaining) {
        at->traversing = false;
        at->offset = end;
        at->previous_high = incoming.part.high;
        break;
      }
      at->working.low = chunk.high;
    }
  }
  return at->offset == packet.size ? lb_recon_status_complete : lb_recon_status_more;
}

typedef char lb_recon_layout_check[(sizeof(lb_recon_item)==16 && sizeof(lb_recon_workspace)==1680)?1:-1];
#ifdef __cplusplus
#include "cpp/reconcile.h"
#endif
#endif
