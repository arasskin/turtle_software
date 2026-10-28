#pragma once
namespace reconciliation {
using range = lb_recon_range;
using summary = lb_recon_summary;
using item = lb_recon_item;
using source = lb_recon_source;
using bytes = lb_recon_bytes;
using output = lb_recon_output;
using differences = lb_recon_differences;
using cursor = lb_recon_cursor;
using workspace = lb_recon_workspace;
using frame = lb_recon_frame;
struct role {
  static constexpr auto initiator = lb_recon_role_initiator;
  static constexpr auto responder = lb_recon_role_responder;
};
struct status {
  static constexpr auto complete = lb_recon_status_complete;
  static constexpr auto more = lb_recon_status_more;
  static constexpr auto output_full = lb_recon_status_output_full;
  static constexpr auto differences_full = lb_recon_status_differences_full;
  static constexpr auto invalid_input = lb_recon_status_invalid_input;
  static constexpr auto source_error = lb_recon_status_source_error;
  static constexpr auto invalid_argument = lb_recon_status_invalid_argument;
};
struct kind {
  static constexpr auto summary = lb_recon_kind_summary;
  static constexpr auto offer = lb_recon_kind_offer;
  static constexpr auto answer = lb_recon_kind_answer;
};
constexpr size_t ID_BYTES = lb_recon_ID_BYTES;
constexpr size_t FINGERPRINT_BYTES = lb_recon_FINGERPRINT_BYTES;
constexpr size_t LEAF_IDS = lb_recon_LEAF_IDS;
constexpr size_t FANOUT = lb_recon_FANOUT;
constexpr size_t MAX_SPLIT_BYTES = lb_recon_MAX_SPLIT_BYTES;
constexpr size_t MAX_LIST_BYTES = lb_recon_MAX_LIST_BYTES;
constexpr size_t MIN_OUTPUT_CAPACITY = lb_recon_MIN_OUTPUT_CAPACITY;
inline constexpr const auto &MAGIC = lb_recon_MAGIC;
inline bool valid(range r) {
  return lb_recon_valid(r);
}
inline bool contains(range r, uint64_t id) {
  return lb_recon_contains(r, id);
}
inline bool upper_bound(range r, uint64_t *out) {
  return lb_recon_upper_bound(r, out);
}
inline size_t integer_bytes(uint64_t value) {
  return lb_recon_integer_bytes(value);
}
inline size_t suffix_bytes(range r) {
  return lb_recon_suffix_bytes(r);
}
inline size_t frame_size(range r, lb_recon_kind type, size_t count = 0) {
  return lb_recon_frame_size(r, type, count);
}
inline void write_integer(output *out, uint64_t value, size_t width) {
  return lb_recon_write_integer(out, value, width);
}
inline void write_frame(output *out, range r, lb_recon_kind type, const item *items, size_t count,
                        uint64_t fingerprint = 0) {
  return lb_recon_write_frame(out, r, type, items, count, fingerprint);
}
inline bool take(bytes packet, size_t *position, size_t width, uint64_t *value) {
  return lb_recon_take(packet, position, width, value);
}
inline bool read_frame(bytes packet, size_t *position, frame *f, workspace *w,
                       uint64_t previous_high = 0) {
  return lb_recon_read_frame(packet, position, f, w, previous_high);
}
inline bool read_own(source src, range r, workspace *w, size_t *count) {
  return lb_recon_read_own(src, r, w, count);
}
inline bool initialize_output(output *out) {
  return lb_recon_initialize_output(out);
}
inline bool room(const output *out, size_t needed) {
  return lb_recon_room(out, needed);
}
inline lb_recon_status refine(source src, range parent, uint64_t count, workspace *w, output *out) {
  return lb_recon_refine(src, parent, count, w, out);
}
inline lb_recon_status begin(source src, workspace *w, output *out) {
  return lb_recon_begin(src, w, out);
}
inline lb_recon_status receive(source src, lb_recon_role side, bytes packet, cursor *at,
                               workspace *w, output *out, differences *diff,
                               size_t work_budget = 64) {
  return lb_recon_receive(src, side, packet, at, w, out, diff, work_budget);
}
} // namespace reconciliation
