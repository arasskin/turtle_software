#include "check.h"
#include "index.h"
#include <algorithm>
#include <map>
#include <vector>

bool operator==(ct_record a, ct_record b) {
  return a.key == b.key && a.value == b.value && a.hash == b.hash;
}
#include <random>

namespace {
struct controlled_allocator {
  ct_pool *pool;
  size_t *budget;
};
void *allocate(void *context, size_t bytes) {
  auto *a = static_cast<controlled_allocator *>(context);
  if (*a->budget != SIZE_MAX) {
    if (!*a->budget)
      return nullptr;
    --*a->budget;
  }
  return ct_pool_allocate(a->pool, bytes);
}
void release(void *context, void *p, size_t bytes) {
  ct_pool_deallocate(static_cast<controlled_allocator *>(context)->pool, p, bytes);
}
uint64_t ranks(uint64_t k) { return k == UINT64_MAX ? 0 : lb_key_hash(k); }
ct_record entry(uint64_t k) { return {k, k ^ 73, lb_key_hash(k ^ 123)}; }
std::vector<ct_record> snapshot(const ct_tree *tree) {
  std::vector<ct_record> records(ct_node_count(tree->root));
  CHECK(ct_tree_list(tree, {}, records.data(), records.size()) == records.size());
  return records;
}

// Fail each allocation along successful insert/delete paths, including joining
// neighbors and promotion to the maximum rank. Every failure preserves content.
void failures() {
  std::mt19937_64 rng(174);
  std::vector<ct_record> base;
  for (size_t i = 0; i < 512; ++i)
    base.push_back(entry(rng()));
  base.push_back(entry(UINT64_MAX));
  size_t insert_failures = 0, delete_failures = 0;
  for (bool removing : {false, true})
    for (size_t trial = 0; trial < 80; ++trial) {
      const uint64_t key = trial == 0 ? UINT64_MAX : removing ? base[trial].key : rng();
      bool completed = false;
      for (size_t allowed = 0; allowed < 64 && !completed; ++allowed) {
        ct_pool pool{};
        ct_pool_init(&pool, nullptr);
        size_t budget = SIZE_MAX;
        {
          controlled_allocator allocator{&pool, &budget};
          ct_tree tree{};
          tree.key_hash = ranks;
          tree.allocator = {&allocator, allocate, release};
          for (auto r : base)
            if (removing || r.key != key)
              CHECK(ct_tree_set(&tree, r) == 1);
          const auto before = snapshot(&tree);
          const auto shape = ct_tree_shape(tree.root, tree.rank);
          budget = allowed;
          int result = removing ? ct_tree_erase(&tree, key) : ct_tree_set(&tree, entry(key));
          if (result >= 0) {
            CHECK(result == 1);
            completed = true;
          } else {
            CHECK(snapshot(&tree) == before);
            CHECK(ct_tree_shape(tree.root, tree.rank) == shape);
            (removing ? delete_failures : insert_failures)++;
          }
          CHECK(ct_tree_validate(&tree));
          auto expected = before;
          if (completed) {
            if (removing)
              std::erase_if(expected, [&](auto r) { return r.key == key; });
            else {
              expected.push_back(entry(key));
              std::sort(expected.begin(), expected.end(),
                        [](auto a, auto b) { return a.key < b.key; });
            }
          }
          CHECK(snapshot(&tree) == expected);
          ct_tree_destroy(&tree);
        }
        CHECK(pool.stats.live_bytes == 0);
        ct_pool_trim(&pool);
        CHECK(pool.stats.reserved_bytes == 0);
        ct_pool_destroy(&pool);
      }
      CHECK(completed);
    }
  CHECK(insert_failures > 10 && delete_failures > 0);
  std::printf("PASS: failed %zu insert and %zu delete allocations without losing records\n",
              insert_failures, delete_failures);
}

void model() {
  ct_pool pool{};
  ct_pool_init(&pool, nullptr);
  auto h = lb_index_create(&pool);
  std::map<uint64_t, ct_record> expected;
  std::mt19937_64 rng(857);
  for (size_t i = 0; i < 20000; ++i) {
    uint64_t k = rng() % 2000;
    if (i % 13 == 0)
      k = UINT64_MAX;
    if (rng() % 3) {
      ct_record r{k, rng() | 1, rng()};
      if (expected.contains(k))
        CHECK(lb_index_replace(&h, k, r.value, r.hash));
      else
        CHECK(lb_index_set(&h, k, r.value, r.hash) == 1);
      expected[k] = r;
    } else
      CHECK(lb_index_erase(&h, k) == (expected.erase(k) ? 1 : 0));
    if (i % 53 == 0) {
      auto tree = lb_index_view(&h);
      CHECK(ct_tree_validate(&tree));
      CHECK(ct_node_count(h.root) == expected.size());
      std::vector<ct_record> records;
      uint64_t hash = 0;
      for (auto [key, r] : expected) {
        records.push_back(r);
        hash ^= r.hash;
        ct_record actual{};
        CHECK(lb_index_find(&h, key, &actual) && actual == r);
      }
      CHECK(snapshot(&tree) == records);
      CHECK(ct_node_hash(h.root) == hash);
      for (size_t rank = 0; rank < records.size(); ++rank) {
        ct_record actual{};
        CHECK(lb_index_select(&h, rank, &actual) && actual == records[rank]);
      }
      const auto summary = lb_index_summary(&h, {100, 1000, true});
      uint64_t range_hash = 0, count = 0;
      for (auto [key, r] : expected)
        if (key >= 100 && key < 1000) {
          range_hash ^= r.hash;
          ++count;
        }
      CHECK(summary.hash == range_hash && summary.count == count);
      std::vector<ct_record> range(count);
      CHECK(ct_tree_list(&tree, {100, 1000, true}, range.data(), range.size()) == count);
      size_t position = 0;
      for (auto [key, r] : expected)
        if (key >= 100 && key < 1000)
          CHECK(range[position++] == r);
    }
  }
  lb_index_destroy(&h);
  CHECK(!h.root && pool.stats.live_bytes == 0);
  ct_pool_trim(&pool);
  CHECK(pool.stats.reserved_bytes == 0);
  ct_pool_destroy(&pool);
}
} // namespace
int main() {
  failures();
  model();
  std::puts(
      "PASS: mutable slab-wide index, counts/ranges/rank selection, and 20,000 model operations");
}
