#include "check.h"
#include "heap.h"
#include <algorithm>
#include <map>
#include <random>
#include <vector>

namespace heap_tests {

void sizes() {
  // Independent, explicit sequence of allowed physical sizes.
  std::vector<uint32_t> slots{32};
  for (uint32_t base = 32; base < 65536; base *= 2)
    for (uint32_t size = base + base / 8; size <= base * 2; size += base / 8)
      slots.push_back(size);
  for (uint32_t bytes = 1; bytes <= UINT16_MAX; ++bytes) {
    auto expected = *std::lower_bound(slots.begin(), slots.end(), bytes);
    auto actual = lb_slot_size(lb_size_class(bytes));
    if (actual != expected)
      std::fprintf(stderr, "request=%u expected=%u actual=%u\n", bytes, expected, unsigned(actual));
    CHECK(actual == expected);
    const auto index = lb_size_class(bytes);
    CHECK(index == std::lower_bound(slots.begin(), slots.end(), bytes) - slots.begin());
    CHECK(lb_slot_size(index) == expected);
  }
  CHECK(slots.size() == LB_SIZE_CLASSES);
  for (size_t invalid : {size_t(0), size_t(65536), SIZE_MAX}) {
    CHECK(lb_size_class(invalid) == LB_INVALID_CLASS);
    CHECK(lb_slot_size(lb_size_class(invalid)) == 0);
  }
  CHECK(lb_slot_size(LB_INVALID_CLASS) == 0);
  CHECK(lb_slot_size(LB_SIZE_CLASSES) == 0);
  std::puts("PASS: all 65,535 supported request sizes");
}

lb_heap create(uint64_t limit, int metadata_bytes = 32) {
  lb_heap a{};
  a.limit = limit;
  for (auto &head : a.size_class_heads)
    head.next_slab_index = -1;
  a.slab_allocator = ct_arena_create(sizeof(lb_heap_slot), metadata_bytes);
  CHECK(!ct_arena_bad(&a.slab_allocator));
  return a;
}

void every_size_reuses_class() {
  size_t first_size = 1;
  for (uint8_t c = 0; c < LB_SIZE_CLASSES; ++c) {
    const uint32_t physical = lb_slot_size(c);
    auto a = create(physical);
    for (size_t size = first_size; size <= std::min(size_t(physical), size_t(LB_MAX_VALUE));
         ++size) {
      auto allocation = lb_heap_reserve(&a, size);
      CHECK(!allocation.has_error && allocation.offset == 0);
      CHECK(a.high_watermark == physical);
      CHECK(lb_heap_reserve(&a, size).has_error);
      CHECK(lb_heap_release(&a, allocation.offset, size));
    }
    first_size = physical + 1;
    ct_arena_destroy(&a.slab_allocator);
  }
}

void capacity_and_reuse() {
  auto a = create(32);
  CHECK(lb_heap_reserve(&a, 0).has_error);
  CHECK(lb_heap_reserve(&a, 65536).has_error);
  CHECK(lb_heap_reserve(&a, SIZE_MAX).has_error);
  CHECK(a.high_watermark == 0);
  auto first = lb_heap_reserve(&a, 32);
  CHECK(!first.has_error && first.offset == 0);
  CHECK(lb_heap_reserve(&a, 1).has_error);
  CHECK(lb_heap_release(&a, first.offset, 32));
  CHECK(lb_heap_reserve(&a, 33).has_error); // another class cannot consume this slot
  auto reused = lb_heap_reserve(&a, 1);
  CHECK(!reused.has_error && reused.offset == 0);
  CHECK(!lb_heap_release(&a, 0, 0));
  CHECK(!lb_heap_release(&a, 0, 65536));
  CHECK(!lb_heap_release(&a, 32, 32));
  ct_arena_destroy(&a.slab_allocator);

  a = create(65535);
  CHECK(lb_heap_reserve(&a, 65535).has_error);
  a.limit = 65536;
  CHECK(!lb_heap_reserve(&a, 65535).has_error);
  CHECK(a.high_watermark == 65536);
  CHECK(lb_heap_release(&a, 0, 65535));
  CHECK(!lb_heap_reserve(&a, 65535).has_error);
  a.high_watermark = UINT64_MAX - 15;
  a.limit = UINT64_MAX;
  CHECK(lb_heap_reserve(&a, 32).has_error); // addition must not wrap
  ct_arena_destroy(&a.slab_allocator);
}

void randomized_reuse() {
  auto a = create(16 * 1024 * 1024);
  std::mt19937_64 random(0xD0D0);
  std::vector<std::pair<uint64_t, size_t>> live;
  std::map<uint64_t, uint32_t> intervals;
  for (size_t step = 0; step < 30000; ++step) {
    if (!live.empty() && (live.size() >= 512 || random() % 2 == 0)) {
      const size_t i = random() % live.size();
      CHECK(lb_heap_release(&a, live[i].first, live[i].second));
      CHECK(intervals.erase(live[i].first) == 1);
      live[i] = live.back();
      live.pop_back();
    } else {
      const size_t size = 1 + random() % 65535;
      const auto allocation = lb_heap_reserve(&a, size);
      if (allocation.has_error)
        continue;
      const uint32_t physical = lb_slot_size(lb_size_class(size));
      auto next = intervals.lower_bound(allocation.offset);
      if (next != intervals.end())
        CHECK(allocation.offset + physical <= next->first);
      if (next != intervals.begin()) {
        auto previous = std::prev(next);
        CHECK(previous->first + previous->second <= allocation.offset);
      }
      CHECK(intervals.emplace(allocation.offset, physical).second);
      live.emplace_back(allocation.offset, size);
    }
  }
  for (auto [offset, size] : live)
    CHECK(lb_heap_release(&a, offset, size));
  // Force metadata growth and verify all saved offsets survive relocation.
  std::vector<uint64_t> offsets;
  for (size_t i = 0; i < 4096; ++i) {
    auto allocation = lb_heap_reserve(&a, 32);
    CHECK(!allocation.has_error);
    offsets.push_back(allocation.offset);
  }
  for (auto offset : offsets)
    CHECK(lb_heap_release(&a, offset, 32));
  const auto watermark = a.high_watermark;
  for (auto i = offsets.rbegin(); i != offsets.rend(); ++i) {
    auto allocation = lb_heap_reserve(&a, 32);
    CHECK(!allocation.has_error && allocation.offset == *i);
  }
  CHECK(a.high_watermark == watermark);
  ct_arena_destroy(&a.slab_allocator);
}

void metadata_growth_and_failure() {
  auto a = ct_arena_create(16, 32);
  CHECK(ct_arena_reserve(&a) == 0);
  CHECK(ct_arena_reserve(&a) == 1);
  ct_arena_release(&a, 0);
  ct_arena_release(&a, 1);
  CHECK(ct_arena_grow(&a));
  CHECK(ct_arena_reserve(&a) == 1);
  CHECK(ct_arena_reserve(&a) == 0);
  CHECK(ct_arena_reserve(&a) == 2);
  a.size = INT_MAX;
  a.high_watermark_index = INT_MAX;
  CHECK(!ct_arena_grow(&a));
  CHECK(a.size == INT_MAX);
  CHECK(ct_arena_reserve(&a) == -1);
  ct_arena_destroy(&a);
  CHECK(ct_arena_reserve(&a) == -1);
  a = ct_arena_create(0, 32);
  CHECK(ct_arena_bad(&a));

  auto h = create(32);
  CHECK(!lb_heap_reserve(&h, 32).has_error);
  const int capacity = h.slab_allocator.size;
  h.slab_allocator.size = INT_MAX;
  h.slab_allocator.high_watermark_index = INT_MAX;
  CHECK(!lb_heap_release(&h, 0, 32));
  CHECK(h.size_class_heads[0].next_slab_index == -1);
  CHECK(lb_heap_reserve(&h, 32).has_error);
  h.slab_allocator.size = capacity;
  h.slab_allocator.high_watermark_index = 0;
  CHECK(lb_heap_release(&h, 0, 32));
  CHECK(!lb_heap_reserve(&h, 32).has_error);
  ct_arena_destroy(&h.slab_allocator);
}

void file_lifetime() {
  char name[] = "/tmp/littlebear-heap-test-XXXXXX";
  int fd = mkstemp(name);
  CHECK(fd >= 0);
  close(fd);
  auto file = lb_file_create(name, 1024);
  unlink(name);
  CHECK(!file.has_error);
  CHECK(fcntl(file.file_descriptor, F_GETFD) & FD_CLOEXEC);
  CHECK(!lb_heap_reserve(&file.file_allocator, 1).has_error);
  lb_file_destroy(&file);
  CHECK(file.file_descriptor == -1);
  CHECK(file.file_allocator.slab_allocator.start == nullptr);
}
} // namespace heap_tests

int main() {
  heap_tests::sizes();
  heap_tests::every_size_reuses_class();
  heap_tests::capacity_and_reuse();
  heap_tests::randomized_reuse();
  heap_tests::metadata_growth_and_failure();
  heap_tests::file_lifetime();
  std::puts("PASS: heap capacity, reuse, metadata growth/failure, and file lifetime");
}
