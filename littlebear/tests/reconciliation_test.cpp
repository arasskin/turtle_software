#include "check.h"
#include "reconcile.h"
#include <algorithm>
#include <array>
#include <map>
#include <random>
#include <set>
#include <vector>

namespace recon_tests {
uint64_t mix(uint64_t x);
struct snapshot {
  std::vector<uint64_t> ids, prefix;
  size_t summary_calls = 0, select_calls = 0, list_calls = 0;
  std::map<uint64_t, uint64_t> fingerprints{};
};
void seal(snapshot *s) {
  std::sort(s->ids.begin(), s->ids.end());
  s->ids.erase(std::unique(s->ids.begin(), s->ids.end()), s->ids.end());
  s->prefix.assign(1, 0);
  for (auto id : s->ids) {
    const auto hash = s->fingerprints.try_emplace(id, mix(id)).first->second;
    s->prefix.push_back(s->prefix.back() ^ hash);
  }
}
size_t lower(snapshot *s, uint64_t key) {
  return size_t(std::lower_bound(s->ids.begin(), s->ids.end(), key) - s->ids.begin());
}
bool summarize(void *context, lb_recon_range part, lb_recon_summary *out) {
  auto *s = static_cast<snapshot *>(context);
  ++s->summary_calls;
  const size_t first = lower(s, part.low), end = part.high ? lower(s, part.high) : s->ids.size();
  *out = {end - first, s->prefix[first] ^ s->prefix[end]};
  return true;
}
bool select(void *context, uint64_t rank, uint64_t *out) {
  auto *s = static_cast<snapshot *>(context);
  ++s->select_calls;
  if (rank >= s->ids.size())
    return false;
  *out = s->ids[size_t(rank)];
  return true;
}
size_t list(void *context, lb_recon_range part, lb_recon_item *out, size_t capacity) {
  auto *s = static_cast<snapshot *>(context);
  ++s->list_calls;
  size_t n = 0;
  for (size_t i = lower(s, part.low);
       i < s->ids.size() && n < capacity && lb_recon_contains(part, s->ids[i]); ++i)
    out[n++] = {s->ids[i], s->fingerprints.at(s->ids[i])};
  return n;
}
lb_recon_source source(snapshot *s) { return {s, summarize, select, list}; }
uint64_t mix(uint64_t x) {
  x += UINT64_C(0x9e3779b97f4a7c15);
  x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
  return x ^ (x >> 31);
}
struct stats {
  size_t turns = 0, bytes = 0, calls = 0;
};
using packet = std::vector<uint8_t>;
stats exchange(snapshot *a, snapshot *b, size_t capacity = 4096, size_t budget = 64) {
  seal(a);
  seal(b);
  std::set<uint64_t> have[2], need[2];
  lb_recon_workspace work{};
  std::vector<uint8_t> memory(capacity);
  lb_recon_output out{memory.data(), memory.size(), 0, 0};
  CHECK(lb_recon_begin(source(a), &work, &out) == lb_recon_status_complete);
  std::vector<packet> packets{{memory.begin(), memory.begin() + out.size}};
  bool to_b = true;
  stats result;
  while (!packets.empty()) {
    CHECK(++result.turns < 100);
    std::vector<packet> replies;
    for (const auto &input : packets) {
      result.bytes += input.size();
      lb_recon_cursor cursor{};
      uint64_t have_ids[lb_recon_LEAF_IDS], need_ids[lb_recon_LEAF_IDS];
      lb_recon_differences diff{have_ids, lb_recon_LEAF_IDS, 0, need_ids, lb_recon_LEAF_IDS, 0};
      for (;;) {
        const auto before = cursor;
        auto code = lb_recon_receive(
            source(to_b ? b : a), to_b ? lb_recon_role_responder : lb_recon_role_initiator,
            {input.data(), input.size()}, &cursor, &work, &out, &diff, budget);
        ++result.calls;
        CHECK(code == lb_recon_status_complete || code == lb_recon_status_more ||
              code == lb_recon_status_output_full || code == lb_recon_status_differences_full);
        const size_t side = to_b ? 1 : 0;
        for (size_t i = 0; i < diff.have_count; ++i)
          CHECK(have[side].insert(diff.have[i]).second);
        for (size_t i = 0; i < diff.need_count; ++i)
          CHECK(need[side].insert(diff.need[i]).second);
        if (out.size > sizeof(lb_recon_MAGIC))
          replies.emplace_back(memory.begin(), memory.begin() + out.size);
        if (code == lb_recon_status_complete)
          break;
        CHECK((cursor.offset > before.offset || cursor.working.low != before.working.low ||
               cursor.working.high != before.working.high ||
               cursor.traversing != before.traversing) &&
              cursor.offset < input.size());
      }
    }
    packets = std::move(replies);
    to_b = !to_b;
  }
  std::map<uint64_t, uint64_t> left, right;
  for (auto id : a->ids)
    left[id] = a->fingerprints.at(id);
  for (auto id : b->ids)
    right[id] = b->fingerprints.at(id);
  std::set<uint64_t> expected_have, expected_need;
  for (const auto &[id, hash] : left)
    if (!right.contains(id) || right.at(id) != hash)
      expected_have.insert(id);
  for (const auto &[id, hash] : right)
    if (!left.contains(id) || left.at(id) != hash)
      expected_need.insert(id);
  CHECK(have[0] == expected_have && need[0] == expected_need);
  CHECK(have[1].empty() && need[1].empty());
  return result;
}
void scenarios() {
  snapshot empty{}, singleton{{0}, {}}, boundary{{0, UINT64_MAX, UINT64_C(0x8000000000000000)}, {}};
  exchange(&empty, &empty);
  exchange(&empty, &singleton);
  exchange(&singleton, &empty);
  exchange(&empty, &boundary);
  exchange(&boundary, &boundary);
  snapshot a, b;
  for (uint64_t i = 0; i < 12000; ++i) {
    a.ids.push_back(mix(i));
    b.ids.push_back(mix(i + 4000));
  }
  exchange(&a, &b, lb_recon_MIN_OUTPUT_CAPACITY, 1);
  exchange(&a, &empty);
  exchange(&empty, &a);
  auto unchanged = exchange(&a, &a);
  CHECK(unchanged.turns == 1 && unchanged.bytes == 14);
  b = a;
  b.ids.back() = mix(999999);
  exchange(&a, &b);
  // Dense shared prefix stresses refinement depth and prefix-compressed lists.
  a.ids.clear();
  b.ids.clear();
  a.fingerprints.clear();
  b.fingerprints.clear();
  for (uint64_t i = 0; i < 1000; ++i) {
    a.ids.push_back(UINT64_C(0xfffffffffff00000) | (mix(i) & 0xfffff));
    b.ids.push_back(UINT64_C(0xfffffffffff00000) | (mix(i + 100) & 0xfffff));
  }
  exchange(&a, &b, lb_recon_MIN_OUTPUT_CAPACITY, 2);
  std::mt19937_64 rng(0xBEA8);
  for (size_t trial = 0; trial < 200; ++trial) {
    a.ids.clear();
    b.ids.clear();
    a.fingerprints.clear();
    b.fingerprints.clear();
    const size_t n = 1 + rng() % 800;
    for (size_t i = 0; i < n; ++i) {
      const auto id = mix(i + trial * 1000);
      if (rng() % 4 != 0)
        a.ids.push_back(id);
      if (rng() % 4 != 0) {
        b.ids.push_back(id);
        if (rng() % 5 == 0)
          b.fingerprints[id] = mix(id ^ UINT64_C(0xabcd1234));
      }
    }
    exchange(&a, &b, trial % 2 ? 4096 : lb_recon_MIN_OUTPUT_CAPACITY, 1 + trial % 17);
  }
}
void content_changes() {
  for (size_t size : {size_t(1), size_t(32), size_t(33), size_t(1000)}) {
    snapshot a, b;
    for (size_t i = 0; i < size; ++i)
      a.ids.push_back(i);
    a.ids.back() = UINT64_MAX;
    seal(&a);
    b = a;
    b.fingerprints[a.ids[size / 2]] ^= mix(777);
    exchange(&a, &b, lb_recon_MIN_OUTPUT_CAPACITY, 1);
    exchange(&b, &a, lb_recon_MIN_OUTPUT_CAPACITY, 1);
    for (auto id : b.ids)
      b.fingerprints[id] = mix(id ^ UINT64_C(0x55555555));
    exchange(&a, &b, lb_recon_MIN_OUTPUT_CAPACITY, 1);
  }
  snapshot a{{0, UINT64_MAX}, {}}, b{{0, UINT64_MAX}, {}};
  a.fingerprints = {{0, 0}, {UINT64_MAX, UINT64_MAX}};
  b.fingerprints = {{0, UINT64_MAX}, {UINT64_MAX, 0}};
  exchange(&a, &b); // Equal aggregate XOR; explicit pairs still detect both edits.
}
void encoding() {
  uint8_t memory[1024];
  lb_recon_output out{memory, sizeof(memory), 0, 0};
  lb_recon_workspace work{};
  auto roundtrip = [&](lb_recon_range part) {
    CHECK(lb_recon_valid(part));
    lb_recon_item items[] = {{part.low, 0}, {part.high ? part.high - 1 : UINT64_MAX, UINT64_MAX}};
    const size_t count = items[0].id == items[1].id ? 1 : 2;
    for (auto kind : {lb_recon_kind_offer, lb_recon_kind_answer, lb_recon_kind_summary}) {
      out.size = 0;
      out.previous_high = 0;
      lb_recon_write_frame(&out, part, kind, items, count, UINT64_MAX);
      CHECK(out.size == lb_recon_frame_size(part, kind, count));
      size_t pos = 0;
      lb_recon_frame frame{};
      CHECK(lb_recon_read_frame({memory, out.size}, &pos, &frame, &work, 0));
      CHECK(pos == out.size && frame.part.low == part.low && frame.part.high == part.high &&
            frame.type == kind);
      if (kind == lb_recon_kind_summary)
        CHECK(frame.fingerprint == UINT64_MAX);
      else {
        CHECK(frame.count == count);
        for (size_t i = 0; i < count; ++i)
          CHECK(work.remote[i].id == items[i].id &&
                work.remote[i].fingerprint == items[i].fingerprint);
      }
    }
  };
  roundtrip({});
  roundtrip({0, 1});
  roundtrip({UINT64_MAX, 0});
  for (size_t width = 0; width <= 8; ++width) {
    const uint64_t low = width == 8 ? 0 : UINT64_MAX << (width * 8);
    CHECK(lb_recon_suffix_bytes({low, 0}) == width);
    roundtrip({low, 0});
  }
  for (uint64_t i = 0; i < 1000; ++i) {
    uint64_t low = mix(i), high = mix(i + 1000);
    if (low > high)
      std::swap(low, high);
    if (low != high)
      roundtrip({low, high});
    roundtrip({low, low + 1});
  }
  CHECK(lb_recon_frame_size({UINT64_MAX, 0}, lb_recon_kind_offer, 1) == 19);
  CHECK(lb_recon_frame_size({}, lb_recon_kind_offer, 32) == 515);
  uint64_t high;
  CHECK(!lb_recon_upper_bound({}, &high));
  CHECK(!lb_recon_upper_bound({UINT64_MAX, 0}, &high));
  CHECK(lb_recon_upper_bound({0, 1}, &high) && high == 1);
}
// Uneven counts and tightly clustered IDs must still produce 16 balanced,
// contiguous ranges, including the peer-only gaps before/after our records.
void counted_splits() {
  snapshot s;
  const uint64_t base = UINT64_C(0xffffffffffff0000);
  s.ids = {0, 1, UINT64_MAX};
  for (uint64_t i = 0; i < 1003; ++i)
    s.ids.push_back(base + i * 3);
  seal(&s);
  uint8_t input[64], memory[lb_recon_MIN_OUTPUT_CAPACITY];
  lb_recon_workspace work{};
  lb_recon_output out{memory, sizeof(memory), 0, 0}, request{input, sizeof(input), 0, 0};
  CHECK(lb_recon_begin(source(&s), &work, &out) == lb_recon_status_complete);
  CHECK(out.size == 14 && s.summary_calls == 1 && s.list_calls == 0 && s.select_calls == 0);
  // Equal roots use cached summaries without listing or selecting any IDs.
  memcpy(input, memory, out.size);
  lb_recon_cursor cursor{};
  lb_recon_differences diff{};
  CHECK(lb_recon_receive(source(&s), lb_recon_role_responder, {input, out.size}, &cursor, &work,
                         &out, &diff, 64) == lb_recon_status_complete);
  CHECK(out.size == 4 && s.list_calls == 0 && s.select_calls == 0);
  for (lb_recon_range parent : {lb_recon_range{}, lb_recon_range{base - 100, UINT64_MAX}}) {
    lb_recon_summary total;
    CHECK(summarize(&s, parent, &total));
    CHECK(lb_recon_initialize_output(&request));
    lb_recon_write_frame(&request, parent, lb_recon_kind_summary, nullptr, 0,
                         total.fingerprint ^ 1);
    cursor = {};
    const auto selects = s.select_calls;
    // A short output must leave the frame pending, without publishing a partial split.
    out.capacity = 14;
    CHECK(lb_recon_receive(source(&s), lb_recon_role_responder, {input, request.size}, &cursor,
                           &work, &out, &diff, 64) == lb_recon_status_output_full);
    CHECK(out.size == 4 && cursor.offset == 4);
    out.capacity = sizeof(memory);
    CHECK(lb_recon_receive(source(&s), lb_recon_role_responder, {input, request.size}, &cursor,
                           &work, &out, &diff, 64) == lb_recon_status_complete);
    CHECK(s.select_calls == selects + 30 && s.list_calls == 0);
    size_t position = 4;
    uint64_t low = parent.low, fingerprint = 0;
    for (size_t i = 0; i < lb_recon_FANOUT; ++i) {
      lb_recon_frame f;
      CHECK(lb_recon_read_frame({memory, out.size}, &position, &f, &work, i ? low : 0));
      CHECK(f.type == lb_recon_kind_summary && f.part.low == low);
      lb_recon_summary child;
      CHECK(summarize(&s, f.part, &child));
      CHECK(child.count == total.count / 16 + (i < total.count % 16));
      CHECK(child.fingerprint == f.fingerprint);
      fingerprint ^= child.fingerprint;
      low = f.part.high;
    }
    CHECK(position == out.size && low == parent.high && fingerprint == total.fingerprint);
  }
  // Small-count/list disagreement and failed rank selection are source errors.
  auto bad = source(&s);
  bad.summarize = [](void *, lb_recon_range, lb_recon_summary *out) {
    *out = {};
    return true;
  };
  CHECK(lb_recon_begin(bad, &work, &out) == lb_recon_status_source_error);
  bad = source(&s);
  bad.select = [](void *, uint64_t, uint64_t *) { return false; };
  cursor = {};
  CHECK(lb_recon_receive(bad, lb_recon_role_responder, {input, request.size}, &cursor, &work, &out,
                         &diff, 64) == lb_recon_status_source_error);
  CHECK(cursor.offset == 4 && out.size == 4);
}
void malformed_and_backpressure() {
  snapshot a{{1, 2}, {}}, b{{3, 4}, {}};
  seal(&a);
  seal(&b);
  uint8_t input[1024], memory[1024];
  lb_recon_workspace work{};
  lb_recon_output first{input, sizeof(input), 0, 0}, out{memory, sizeof(memory), 0, 0};
  CHECK(lb_recon_begin(source(&a), &work, &first) == lb_recon_status_complete);
  lb_recon_cursor at{};
  lb_recon_differences diff{};
  CHECK(lb_recon_receive(source(&b), lb_recon_role_initiator, {input, first.size}, &at, &work, &out,
                         &diff, 64) == lb_recon_status_differences_full);
  CHECK(at.offset == sizeof(lb_recon_MAGIC) && out.size == sizeof(lb_recon_MAGIC));
  uint64_t have[32], need[32];
  diff = {have, 32, 0, need, 32, 0};
  out.capacity = sizeof(lb_recon_MAGIC);
  at = {};
  CHECK(lb_recon_receive(source(&b), lb_recon_role_responder, {input, first.size}, &at, &work, &out,
                         &diff, 64) == lb_recon_status_output_full);
  CHECK(diff.have_count == 0 && diff.need_count == 0 && at.offset == sizeof(lb_recon_MAGIC));
  out.capacity = sizeof(memory);
  at = {};
  CHECK(lb_recon_receive(source(&b), lb_recon_role_initiator, {input, first.size}, &at, &work, &out,
                         &diff, 64) == lb_recon_status_complete);
  CHECK(diff.have_count == 2 && diff.need_count == 2);
  for (size_t len = 0; len < first.size; ++len) {
    if (len == sizeof(lb_recon_MAGIC))
      continue; // a valid empty acknowledgment
    at = {};
    CHECK(lb_recon_receive(source(&b), lb_recon_role_initiator, {input, len}, &at, &work, &out,
                           &diff, 64) == lb_recon_status_invalid_input);
  }
  auto invalid = [&](packet bytes) {
    lb_recon_cursor cursor{};
    CHECK(lb_recon_receive(source(&b), lb_recon_role_initiator, {bytes.data(), bytes.size()},
                           &cursor, &work, &out, &diff, 64) == lb_recon_status_invalid_input);
  };
  invalid({0x61});             // upstream wire marker
  invalid({'L', 'B', 'R', 1}); // previous prefix-only protocol
  invalid({'L', 'B', 'R', 2}); // previous ID-equals-fingerprint protocol
  invalid({'L', 'B', 'R', 3, 99, 0});
  invalid({'L', 'B', 'R', 3, 1, 0xa0}); // boundary wider than uint64
  invalid({'L', 'B', 'R', 3, 1, 0x90}); // no preceding boundary to reuse
  invalid({'L', 'B', 'R', 3, 1, 0, 33});
  invalid({'L', 'B', 'R', 3, 1, 0x10, 0});    // noncanonical zero
  invalid({'L', 'B', 'R', 3, 1, 0x11, 5, 5}); // empty finite range
  invalid({'L', 'B', 'R', 3, 1, 0x11, 5, 4}); // reversed range
  first.size = sizeof(lb_recon_MAGIC);
  lb_recon_item duplicate[] = {{1, 123}, {1, 456}};
  lb_recon_write_frame(&first, {}, lb_recon_kind_offer, duplicate, 2, 0);
  invalid(packet(input, input + first.size));
  // A same-key edit must reserve both result slots before publishing either.
  snapshot local{{7}, {}}, remote{{7}, {}};
  local.fingerprints[7] = 100;
  remote.fingerprints[7] = 200;
  seal(&local);
  seal(&remote);
  CHECK(lb_recon_begin(source(&remote), &work, &first) == lb_recon_status_complete);
  at = {};
  diff = {have, 1, 0, nullptr, 0, 0};
  CHECK(lb_recon_receive(source(&local), lb_recon_role_initiator, {input, first.size}, &at, &work,
                         &out, &diff, 64) == lb_recon_status_differences_full);
  CHECK(at.offset == 4 && diff.have_count == 0 && diff.need_count == 0);
  diff = {have, 1, 0, need, 1, 0};
  CHECK(lb_recon_receive(source(&local), lb_recon_role_initiator, {input, first.size}, &at, &work,
                         &out, &diff, 64) == lb_recon_status_complete);
  CHECK(diff.have_count == 1 && diff.need_count == 1 && have[0] == 7 && need[0] == 7);
  std::mt19937_64 random(1);
  for (size_t i = 0; i < 20000; ++i) {
    packet bytes(4 + random() % 128);
    memcpy(bytes.data(), lb_recon_MAGIC, 4);
    for (size_t j = 4; j < bytes.size(); ++j)
      bytes[j] = uint8_t(random());
    lb_recon_cursor cursor{};
    // Reject or process safely; not all random bytes are necessarily invalid.
    lb_recon_receive(source(&b), lb_recon_role_initiator, {bytes.data(), bytes.size()}, &cursor,
                     &work, &out, &diff, 1);
  }
}
} // namespace recon_tests
int main() {
  recon_tests::encoding();
  recon_tests::scenarios();
  recon_tests::counted_splits();
  recon_tests::content_changes();
  recon_tests::malformed_and_backpressure();
  std::puts(
      "PASS: separate 8-byte IDs/fingerprints, stable-key edits, 200 model exchanges, balanced "
      "counted splits, arbitrary boundaries, resumable bounded buffers, and 20,000 parser inputs");
}
