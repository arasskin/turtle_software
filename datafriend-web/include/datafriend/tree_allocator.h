#pragma once
#include "../../../containers/pool.h"
#include <memory>
#include <utility>
namespace datafriend {
struct malloc_nodes {
  void *allocate(size_t bytes) { return malloc(bytes); }
  void deallocate(void *p, size_t) { free(p); }
};
class slab_pool : public ct_pool {
public:
  using system_allocate_fn = ct_allocate_fn;
  using statistics = ct_pool_stats;
  static constexpr size_t slab_bytes = CT_SLAB_BYTES, max_block_bytes = CT_MAX_BLOCK;
  explicit slab_pool(system_allocate_fn fn = ct_system_allocate) { ct_pool_init(this, fn); }
  slab_pool(const slab_pool &) = delete;
  slab_pool &operator=(const slab_pool &) = delete;
  ~slab_pool() { ct_pool_destroy(this); }
  const statistics &stats() const { return ct_pool::stats; }
  void *allocate(size_t n) { return ct_pool_allocate(this, n); }
  void deallocate(void *p, size_t n) { ct_pool_deallocate(this, p, n); }
  void trim() { ct_pool_trim(this); }
};
struct slab_nodes {
  std::unique_ptr<slab_pool> owner;
  slab_pool *pool;
  slab_nodes() : owner(std::make_unique<slab_pool>()), pool(owner.get()) {}
  explicit slab_nodes(slab_pool &p) : pool(&p) {}
  slab_nodes(slab_nodes &&) = default;
  void *allocate(size_t n) { return pool->allocate(n); }
  void deallocate(void *p, size_t n) { pool->deallocate(p, n); }
};
} // namespace datafriend
