// C++ ownership and container adapter for Littlebear's shared C99 wide tree.
#pragma once
#include "../../../containers/tree.h"
#include "tree_allocator.h"
#include <algorithm>
#include <bit>
#include <optional>
#include <stdexcept>
#include <vector>
inline bool operator==(const ct_record &a, const ct_record &b) {
  return a.key == b.key && a.value == b.value && a.hash == b.hash;
}
namespace datafriend {
using record = ct_record;
struct key_range {
  uint64_t low = 0;
  std::optional<uint64_t> high;
  bool contains(uint64_t k) const { return k >= low && (!high || k < *high); }
  bool empty() const { return high && low >= *high; }
};
using range_summary = ct_summary;
template <class Allocator = malloc_nodes, bool TrackAllocations = true> class basic_wide_tree {
  Allocator allocator_;
  ct_tree_stats stats_{};
  ct_tree tree_{};
  static void *allocate(void *p, size_t n) {
    try {
      return static_cast<Allocator *>(p)->allocate(n);
    } catch (const std::bad_alloc &) {
      return nullptr;
    }
  }
  static void release(void *p, void *memory, size_t n) {
    static_cast<Allocator *>(p)->deallocate(memory, n);
  }
  static ct_range bounds(key_range r) { return {r.low, r.high.value_or(0), r.high.has_value()}; }
  static record convert(ct_record r) { return {r.key, r.value, r.hash}; }

public:
  struct state {
    ct_node *root = nullptr;
    unsigned rank = 0;
  };
  using key_hash_fn = uint64_t (*)(uint64_t);
  using allocation_stats = ct_tree_stats;
  explicit basic_wide_tree(key_hash_fn hash, Allocator allocator = {})
      : allocator_(std::move(allocator)) {
    if (!hash)
      throw std::invalid_argument("key hash required");
    tree_.key_hash = hash;
    tree_.allocator = {&allocator_, allocate, release};
    if constexpr (TrackAllocations)
      tree_.stats = &stats_;
  }
  ~basic_wide_tree() { ct_tree_destroy(&tree_); }
  basic_wide_tree(const basic_wide_tree &) = delete;
  basic_wide_tree &operator=(const basic_wide_tree &) = delete;
  uint64_t size() const { return ct_node_count(tree_.root); }
  uint64_t fingerprint() const { return ct_node_hash(tree_.root); }
  const allocation_stats &allocations() const { return stats_; }
  const Allocator &allocator() const { return allocator_; }
  void adopt_state(state s) {
    assert(!tree_.root);
    tree_.root = s.root;
    tree_.rank = s.rank;
  }
  state take_state() {
    state s{tree_.root, tree_.rank};
    tree_.root = nullptr;
    tree_.rank = 0;
    return s;
  }
  std::optional<record> get(uint64_t k) const {
    ct_record r;
    if (ct_tree_get(&tree_, k, &r))
      return convert(r);
    return {};
  }
  bool set(record r) {
    int rc = ct_tree_set(&tree_, {r.key, r.value, r.hash});
    if (rc < 0)
      throw std::bad_alloc();
    return rc != 0;
  }
  bool replace_existing(record r) {
    return ct_node_replace(tree_.root, tree_.rank, {r.key, r.value, r.hash});
  }
  bool erase(uint64_t k) {
    int rc = ct_tree_erase(&tree_, k);
    if (rc < 0)
      throw std::bad_alloc();
    return rc != 0;
  }
  range_summary summarize(key_range r) const { return ct_tree_summary(&tree_, bounds(r)); }
  std::optional<record> select(uint64_t n) const {
    ct_record r;
    if (ct_tree_select(&tree_, n, &r))
      return convert(r);
    return {};
  }
  std::vector<record> list(key_range r, size_t limit) const {
    const size_t count = std::min<uint64_t>(limit, summarize(r).count);
    std::vector<record> out(count);
    const size_t used = ct_tree_list(&tree_, bounds(r), out.data(), count);
    out.resize(used);
    return out;
  }
  template <class F> void for_each(F &&f) const { visit(tree_.root, tree_.rank, f); }
  std::vector<uint64_t> cuts(key_range r, size_t fanout) const {
    std::vector<uint64_t> out;
    auto n = summarize(r).count;
    if (n < 2 || fanout < 2)
      return out;
    fanout = std::min<uint64_t>(fanout, n);
    auto start = ct_tree_prefix(&tree_, r.low).count;
    for (size_t i = 1; i < fanout; ++i)
      out.push_back(select(start + (n / fanout) * i + std::min<uint64_t>(i, n % fanout))->key);
    return out;
  }
  void validate() const {
    if (!ct_tree_validate(&tree_))
      throw std::logic_error("wide tree invariant");
  }
  uint64_t shape_digest() const { return ct_tree_shape(tree_.root, tree_.rank); }

private:
  template <class F> static void visit(const ct_node *n, unsigned rank, F &f) {
    if (!n)
      return;
    if (rank) {
      for (size_t i = 0; i < n->size; i += 2)
        visit(ct_cdata(n)[i].child, rank - 1, f);
    } else
      for (size_t i = 0; i < n->size; i += 3)
        f(convert(ct_entry(n, i)));
  }
};
using wide_tree = basic_wide_tree<>;
using slab_wide_tree = basic_wide_tree<slab_nodes>;
} // namespace datafriend
