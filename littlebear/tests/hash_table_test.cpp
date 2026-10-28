#include "check.h"
#include "namespaces.h"
#include <map>
#include <random>
#include <vector>

namespace table_tests {
using model_key = std::pair<uint64_t, uint64_t>;
using model = std::map<model_key, lb_index>;
model_key pair(lb_namespace k) { return {k.high64, k.low64}; }
bool same(lb_index a, lb_index b) {
  return a.root == b.root && a.node_allocator == b.node_allocator && a.rank == b.rank;
}
void verify(lb_directory *t, const model &expected) {
  CHECK(t->count == expected.size());
  model found;
  for (size_t i = 0; i < t->count; ++i)
    CHECK(found.emplace(pair(t->entries[i].key), t->entries[i].value).second);
  CHECK(found.size() == expected.size());
  for (auto [k, v] : expected) {
    lb_namespace key{.low64 = k.second, .high64 = k.first};
    auto *got = lb_directory_get(t, key);
    CHECK(got && same(*got, v));
    CHECK(lb_directory_get(t, key));
    CHECK(same(*lb_directory_get(t, key), v));
    CHECK(found.contains(k) && same(found.at(k), v));
  }
}
inline size_t allocation_calls = 0, fail_at = 0;
void *test_allocate(size_t alignment, size_t bytes) {
  if (++allocation_calls == fail_at)
    return nullptr;
  return std::aligned_alloc(alignment, bytes);
}
void presence_and_boundaries() {
  static_assert(sizeof(lb_index) == 24);
  auto t = lb_directory_create(0, test_allocate);
  CHECK(!!t.initialized && allocation_calls == 0);
  CHECK(!t.entries && !t.slots && !t.capacity && !t.buckets);
  model expected;
  const lb_namespace keys[] = {
      {0, 0}, {0, UINT64_MAX}, {UINT64_MAX, 0}, lb_namespace{UINT64_MAX, UINT64_MAX}};
  for (auto key : keys) {
    CHECK(!lb_directory_get(&t, key));
    CHECK(lb_directory_insert(&t, key, {}));
    CHECK(!lb_directory_get(&t, key)->root);
    lb_directory_get(&t, key)->rank = 42;
    expected[pair(key)].rank = 42;
    verify(&t, expected);
  }
  // Existing values can change without allocation.
  allocation_calls = 0;
  fail_at = 1;
  *lb_directory_get(&t, keys[0]) = {};
  expected[pair(keys[0])] = {};
  CHECK(allocation_calls == 0);
  verify(&t, expected);
  for (auto key : keys) {
    CHECK(lb_directory_erase(&t, key));
    CHECK(!lb_directory_erase(&t, key));
    expected.erase(pair(key));
    verify(&t, expected);
  }
  CHECK(allocation_calls == 0);
  fail_at = 0;
  CHECK(lb_directory_insert(&t, lb_namespace{UINT64_MAX, UINT64_MAX}, {}));
  lb_directory_clear(&t);
  verify(&t, {});
  lb_directory_destroy(&t);
  CHECK(!t.initialized);
  CHECK(!lb_directory_insert(&t, {}, {}));
  CHECK(!lb_directory_reserve(&t, 1));
  CHECK(!lb_directory_erase(&t, {}));
  lb_directory_clear(&t);
  lb_directory_destroy(&t);
}
void collisions_and_deletion() {
  auto t = lb_directory_create(0, ct_system_allocate);
  model expected;
  // All share the exact hash. Equality must still use high64, without an early
  // capacity failure from the former table's two-choice collisions.
  for (uint64_t i = 0; i < 512; ++i) {
    lb_namespace k{123, i};
    lb_index v{};
    v.rank = unsigned(i + 1);
    CHECK(lb_directory_insert(&t, k, v));
    expected.emplace(pair(k), v);
  }
  verify(&t, expected);
  for (uint64_t i = 0; i < 512; i += 2) {
    CHECK(lb_directory_erase(&t, {123, i}));
    expected.erase(pair({123, i}));
  }
  verify(&t, expected);
  CHECK(!lb_directory_get(&t, {123, 999}));
  lb_directory_destroy(&t);
}
void allocation_failures() {
  auto invalid = lb_directory_create(SIZE_MAX, ct_system_allocate);
  CHECK(!invalid.initialized);
  invalid = lb_directory_create(0, nullptr);
  CHECK(!invalid.initialized);
  // Fail every allocation in eager creation, then every allocation while
  // lazily building a directory through multiple value and index growths.
  size_t creation_failures = 0, insert_failures = 0, populated_failures = 0;
  for (size_t failure = 1;; ++failure) {
    allocation_calls = 0;
    fail_at = failure;
    auto t = lb_directory_create(1024, test_allocate);
    if (t.initialized) {
      lb_directory_destroy(&t);
      break;
    }
    ++creation_failures;
  }
  CHECK(creation_failures >= 2);
  for (size_t failure = 1;; ++failure) {
    allocation_calls = 0;
    fail_at = failure;
    auto t = lb_directory_create(0, test_allocate);
    model expected;
    bool failed = false;
    std::mt19937_64 random(123);
    for (size_t i = 0; i < 1024; ++i) {
      lb_namespace k{random(), random()};
      lb_index v{};
      v.rank = unsigned(i + 1);
      if (!lb_directory_insert(&t, k, v)) {
        failed = true;
        ++insert_failures;
        populated_failures += !expected.empty();
        verify(&t, expected);
        CHECK(!lb_directory_get(&t, k));
        fail_at = 0;
        CHECK(lb_directory_insert(&t, k, v));
      }
      expected.emplace(pair(k), v);
    }
    verify(&t, expected);
    lb_directory_destroy(&t);
    if (!failed)
      break;
  }
  CHECK(insert_failures > 10 && populated_failures > 5);
  // reserve can relocate values before the index allocation fails. Contents
  // must remain searchable; old value addresses are intentionally not retained.
  size_t reserve_failures = 0;
  for (size_t failure = 1;; ++failure) {
    fail_at = 0;
    auto t = lb_directory_create(0, test_allocate);
    model expected;
    for (uint64_t i = 0; i < 16; ++i) {
      CHECK(lb_directory_insert(&t, {i, i}, {}));
      expected.emplace(pair({i, i}), lb_index{});
    }
    allocation_calls = 0;
    fail_at = failure;
    bool ok = lb_directory_reserve(&t, 4096);
    verify(&t, expected);
    fail_at = 0;
    CHECK(lb_directory_reserve(&t, 4096));
    verify(&t, expected);
    lb_directory_destroy(&t);
    if (ok)
      break;
    ++reserve_failures;
  }
  CHECK(reserve_failures >= 2);
  std::printf("Dense allocation failures checked: create=%zu insert=%zu reserve=%zu\n",
              creation_failures, insert_failures, reserve_failures);
}
void random_operations() {
  auto t = lb_directory_create(1, ct_system_allocate);
  std::mt19937_64 random(0xBEA4);
  std::vector<lb_namespace> keys{lb_namespace{UINT64_MAX, UINT64_MAX}, {0, 0}};
  while (keys.size() < 4096)
    keys.push_back({random(), random()});
  model expected;
  for (size_t step = 0; step < 50000; ++step) {
    auto key = keys[random() % keys.size()];
    switch (random() % 4) {
    case 0:
      CHECK(lb_directory_erase(&t, key) == (expected.erase(pair(key)) != 0));
      break;
    case 1:
    case 2: {
      lb_index v{};
      v.root = reinterpret_cast<ct_node *>(uintptr_t(random()));
      if (auto *entry = lb_directory_get(&t, key))
        *entry = v;
      else
        CHECK(lb_directory_insert(&t, key, v));
      expected[pair(key)] = v;
      break;
    }
    default:
      break;
    }
    auto reference = expected.find(pair(key));
    auto *found = lb_directory_get(&t, key);
    CHECK((found != nullptr) == (reference != expected.end()));
    if (found != nullptr)
      CHECK(same(*found, reference->second));
    CHECK(t.count == expected.size());
    if (step % 997 == 0)
      verify(&t, expected);
  }
  CHECK(t.buckets > 1);
  verify(&t, expected);
  lb_directory_destroy(&t);
}

void grows_past_former_limit() {
  auto t = lb_directory_create(0, ct_system_allocate);
  constexpr size_t total = 262145;
  std::mt19937_64 random(876);
  std::vector<lb_namespace> keys;
  for (size_t i = 0; i < total; ++i) {
    keys.push_back({random(), random()});
    lb_index v{};
    v.rank = unsigned(i + 1);
    CHECK(lb_directory_insert(&t, keys.back(), v));
  }
  CHECK(t.count == total);
  for (size_t i = 0; i < total; ++i)
    CHECK(lb_directory_get(&t, keys[i])->rank == i + 1);
  lb_directory_destroy(&t);
}
void actual_namespaces() {
  ct_pool nodes{};
  ct_pool_init(&nodes, nullptr);
  auto t = lb_directory_create(1, ct_system_allocate);
  const lb_namespace a{1, 2}, b{3, 4};
  CHECK(lb_directory_insert(&t, a, lb_index_create(&nodes)));
  CHECK(lb_directory_insert(&t, b, lb_index_create(&nodes)));
  CHECK(lb_index_set(lb_directory_get(&t, a), 10, 11, UINT64_MAX) == 1);
  CHECK(lb_index_set(lb_directory_get(&t, b), 20, 22, UINT64_MAX) == 1);
  CHECK(lb_directory_reserve(&t, 16));
  CHECK(lb_index_get(lb_directory_get(&t, a), 10) == 11);
  CHECK(lb_index_get(lb_directory_get(&t, a), 20) == 0);
  CHECK(lb_index_get(lb_directory_get(&t, b), 20) == 22);
  CHECK(lb_index_erase(lb_directory_get(&t, a), 10) == 1);
  CHECK(lb_directory_get(&t, a));
  CHECK(!lb_directory_get(&t, a)->root);
  CHECK(lb_directory_erase(&t, a));
  CHECK(lb_directory_get(&t, a) == nullptr);
  CHECK(lb_index_erase(lb_directory_get(&t, b), 20) == 1);
  lb_directory_clear(&t);
  CHECK(lb_directory_get(&t, b) == nullptr);
  lb_directory_destroy(&t);
  CHECK(nodes.stats.live_bytes == 0);
  ct_pool_trim(&nodes);
  CHECK(nodes.stats.reserved_bytes == 0);
  ct_pool_destroy(&nodes);
}
} // namespace table_tests
int main() {
  table_tests::presence_and_boundaries();
  table_tests::collisions_and_deletion();
  table_tests::allocation_failures();
  table_tests::random_operations();
  table_tests::grows_past_former_limit();
  table_tests::actual_namespaces();
  std::puts("PASS: dense namespace presence, full keys, collisions, growth/failure, 50,000 random "
            "operations, and live trees");
}
