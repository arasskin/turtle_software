#include "../../containers/pool.h"
#include "../../containers/tree.h"
#include <cstring>
#include <iostream>
#include <random>
#include <vector>

bool operator==(const ct_record &a, const ct_record &b) {
  return a.key == b.key && a.value == b.value && a.hash == b.hash;
}
namespace {
void require(bool ok, const char *why) {
  if (!ok)
    throw std::runtime_error(why);
}
std::vector<ct_record> snapshot(const ct_tree &t) {
  std::vector<ct_record> v(ct_node_count(t.root));
  ct_tree_list(&t, {}, v.data(), v.size());
  return v;
}
uint64_t key_hash(uint64_t key) { return key * UINT64_C(11400714819323198485); }
} // namespace

int main() {
  try {
    ct_pool pool{};
    ct_pool_init(&pool, nullptr);
    std::vector<size_t> sizes;
    for (size_t cap = 4; cap < 1500; cap += std::max<size_t>(1, cap / 4))
      sizes.push_back(24 + 8 * cap);
    struct block {
      void *p = nullptr;
      size_t bytes = 0;
      uint8_t marker = 0;
    };
    std::vector<block> live(4000);
    std::mt19937_64 rng(735);
    auto release = [&](block &b) {
      if (!b.p)
        return;
      const auto *p = static_cast<uint8_t *>(b.p);
      for (size_t i = 0; i < b.bytes; ++i)
        require(p[i] == b.marker, "slab overlap/corruption");
      ct_pool_deallocate(&pool, b.p, b.bytes);
      b.p = nullptr;
    };
    for (size_t step = 0; step < 40000; ++step) {
      auto &b = live[rng() % live.size()];
      release(b);
      b.bytes = sizes[rng() % sizes.size()];
      b.marker = uint8_t(rng());
      b.p = ct_pool_allocate(&pool, b.bytes);
      require(b.p && reinterpret_cast<uintptr_t>(b.p) % 8 == 0, "slab alignment");
      std::memset(b.p, b.marker, b.bytes);
      if (step % 1000 == 0)
        ct_pool_trim(&pool); // must preserve every live block
    }
    for (auto &b : live)
      release(b);
    require(pool.stats.live_bytes == 0 && pool.stats.fallback_live_bytes == 0, "slab leak");
    require(pool.stats.slab_frees > 0 && pool.stats.fallback_allocations > 0, "unexercised paths");
    ct_pool_trim(&pool);
    require(pool.stats.reserved_bytes == 0, "trim retained slabs");
    // Fill multiple slabs in one class, release/reuse without a system allocation.
    std::vector<void *> blocks;
    for (size_t i = 0; i < 3000; ++i)
      blocks.push_back(ct_pool_allocate(&pool, 56));
    ct_pool_deallocate(&pool, blocks[100], 56);
    const auto allocs = pool.stats.slab_allocations;
    blocks[100] = ct_pool_allocate(&pool, 56);
    require(pool.stats.slab_allocations == allocs, "free slot not reused");
    for (auto *p : blocks)
      ct_pool_deallocate(&pool, p, 56);
    require(pool.stats.reserved_bytes == CT_SLAB_BYTES, "empty slab cache is unbounded");
    ct_pool_trim(&pool);
    {
      ct_tree_stats reference_stats{}, survivor_stats{};
      ct_tree reference{nullptr,
                        0,
                        key_hash,
                        {nullptr, [](void *, size_t n) { return malloc(n); },
                         [](void *, void *p, size_t) { free(p); }},
                        &reference_stats};
      ct_node_allocator allocator{
          &pool, [](void *p, size_t n) { return ct_pool_allocate(static_cast<ct_pool *>(p), n); },
          [](void *p, void *b, size_t n) { ct_pool_deallocate(static_cast<ct_pool *>(p), b, n); }};
      ct_tree survivor{nullptr, 0, key_hash, allocator, &survivor_stats};
      for (size_t i = 0; i < 2500; ++i) {
        auto r = ct_record{key_hash(i), i + 1, i + 1};
        require(ct_tree_set(&survivor, r) == 1 && ct_tree_set(&reference, r) == 1, "tree insert");
      }
      {
        ct_tree temporary{nullptr, 0, key_hash, allocator, nullptr};
        for (size_t i = 0; i < 2500; ++i)
          require(ct_tree_set(&temporary, ct_record{key_hash(i), i + 7, i + 7}) == 1,
                  "temporary insert");
        require(ct_tree_validate(&temporary), "tree validation");
        ct_tree_destroy(&temporary);
      }
      ct_pool_trim(&pool);
      require(ct_tree_validate(&survivor), "tree validation");
      require(snapshot(survivor) == snapshot(reference), "shared slab lifetime");
      require(ct_tree_shape(survivor.root, survivor.rank) ==
                  ct_tree_shape(reference.root, reference.rank),
              "allocator changed shape");
      require(survivor_stats.copied_bytes == reference_stats.copied_bytes,
              "allocator changed copies");
      require(ct_tree_erase(&survivor, key_hash(71)) == 1 &&
                  ct_tree_set(&survivor, ct_record{UINT64_MAX, 7, 7}) == 1,
              "survivor edit");
      require(ct_tree_erase(&reference, key_hash(71)) == 1 &&
                  ct_tree_set(&reference, {UINT64_MAX, 7, 7}) == 1,
              "reference edit");
      require(snapshot(survivor) == snapshot(reference), "shared root edits");
      ct_tree_destroy(&survivor);
      ct_tree_destroy(&reference);
    }
    require(pool.stats.live_bytes == 0, "shared roots leaked");
    ct_pool_trim(&pool);
    require(pool.stats.reserved_bytes == 0, "shared trim failed");
    ct_pool_destroy(&pool);
    std::cout << "PASS: slab overlap/alignment, every size class, fallback, reuse, reclamation, "
                 "shared roots and edits\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
