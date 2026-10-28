#include "check.h"
#include "storage.h"
#ifdef __linux__
#include "storage_uring.h"
#endif
#include <algorithm>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace storage_tests {
const lb_namespace A{1, 2}, B{3, 4};
struct fixture {
  lb_store s{};
  char path[64] = "/tmp/littlebear-storage-XXXXXX";
};
void create(fixture *f, uint64_t limit = 1024 * 1024, size_t pending = 64) {
  int fd = mkstemp(f->path);
  CHECK(fd >= 0);
  close(fd);
  CHECK(lb_store_create(&f->s, f->path, limit, pending, 1, NULL, ct_system_allocate).code ==
        lb_error_ok);
}
void destroy(fixture *f) {
  CHECK(lb_store_destroy(&f->s).code == lb_error_ok);
  CHECK(unlink(f->path) == 0);
}
uint64_t hash(const std::string &bytes) {
  uint64_t h = UINT64_C(14695981039346656037);
  for (unsigned char b : bytes)
    h = (h ^ b) * UINT64_C(1099511628211);
  return h;
}
lb_store_result drive(lb_store_request *r, size_t chunk = SIZE_MAX) {
  lb_store_result outcome;
  do {
    lb_store_io_operation op{};
    CHECK(lb_store_start_io(r, &op).code == lb_error_ok);
    const size_t length = std::min(size_t(op.length), chunk);
    const ssize_t bytes = op.writing ? pwrite(op.fd, op.buffer, length, op.offset)
                                     : pread(op.fd, op.buffer, length, op.offset);
    outcome = lb_store_complete_io(r, bytes < 0 ? -errno : int(bytes));
  } while (outcome.code == lb_error_more_io);
  CHECK(r->owner == nullptr && r->state == lb_phase_finished);
  return outcome;
}
void put(lb_store *s, lb_namespace ns, uint64_t key, const std::string &bytes,
         size_t chunk = SIZE_MAX) {
  lb_store_request r{};
  CHECK(lb_store_prepare_write(s, &r, ns, key, bytes.data(), bytes.size(), hash(bytes)).code ==
        lb_error_ok);
  CHECK(drive(&r, chunk).code == lb_error_ok);
}
std::string get(lb_store *s, lb_namespace ns, uint64_t key, size_t chunk = SIZE_MAX) {
  char bytes[UINT16_MAX];
  lb_store_request r{};
  CHECK(lb_store_prepare_read(s, &r, ns, key, bytes, sizeof(bytes)).code == lb_error_ok);
  CHECK(drive(&r, chunk).code == lb_error_ok);
  return {bytes, r.transferred};
}
uint64_t offset(lb_store *s, lb_namespace ns, uint64_t key) {
  return lb_voffset(lb_index_get(lb_directory_get(&s->namespaces, ns), key));
}
void lifecycle() {
  fixture f;
  create(&f);
  lb_store_request r{};
  char buffer[100]{};
  CHECK(lb_store_lookup_namespace(&f.s, A) == lb_presence_absent);
  CHECK(lb_store_prepare_read(&f.s, &r, A, 1, buffer, sizeof(buffer)).code ==
        lb_error_namespace_missing);
  CHECK(lb_store_prepare_write(&f.s, &r, A, 1, "a", 1, 1).code == lb_error_namespace_missing);
  CHECK(lb_store_lookup_namespace(&f.s, A) == lb_presence_absent);
  CHECK(lb_store_create_namespace(&f.s, A).code == lb_error_ok);
  CHECK(lb_store_create_namespace(&f.s, A).code == lb_error_ok);
  CHECK(lb_store_lookup_namespace(&f.s, A) == lb_presence_empty);
  CHECK(lb_store_prepare_read(&f.s, &r, A, 1, buffer, sizeof(buffer)).code ==
        lb_error_record_missing);
  for (size_t length : {size_t(0), size_t(65536), SIZE_MAX})
    CHECK(lb_store_prepare_write(&f.s, &r, A, 1, buffer, length, 1).code ==
          lb_error_invalid_argument);
  const std::string binary("a\0b\n\xff", 5);
  put(&f.s, A, 0, binary, 2);
  CHECK(offset(&f.s, A, 0) == 0);
  CHECK(get(&f.s, A, 0, 1) == binary);
  CHECK(lb_store_prepare_read(&f.s, &r, A, 0, buffer, 4).code == lb_error_buffer_too_small);
  CHECK(lb_store_prepare_read(&f.s, &r, A, 0, nullptr, 5).code == lb_error_invalid_argument);
  CHECK(lb_store_create_namespace(&f.s, B).code == lb_error_ok);
  put(&f.s, B, 0, "different");
  put(&f.s, A, UINT64_MAX, std::string(UINT16_MAX, '\x9e'), 997);
  CHECK(get(&f.s, A, UINT64_MAX, 500) == std::string(UINT16_MAX, '\x9e'));
  CHECK(lb_store_clear_namespace(&f.s, A, 1).code == lb_error_more_work);
  CHECK(lb_store_lookup_namespace(&f.s, A) == lb_presence_populated);
  CHECK(lb_store_clear_namespace(&f.s, A, 1).code == lb_error_ok);
  CHECK(lb_store_lookup_namespace(&f.s, A) == lb_presence_empty);
  CHECK(get(&f.s, B, 0) == "different");
  CHECK(lb_store_remove_namespace(&f.s, A, 256).code == lb_error_ok);
  CHECK(lb_store_lookup_namespace(&f.s, A) == lb_presence_absent);
  CHECK(lb_store_remove_namespace(&f.s, A, 256).code == lb_error_namespace_missing);
  CHECK(lb_store_erase_record(&f.s, B, 0).code == lb_error_ok);
  CHECK(lb_store_lookup_namespace(&f.s, B) == lb_presence_empty);
  destroy(&f);
}
void pending_and_replacement() {
  fixture f;
  create(&f, 1024 * 1024, 2);
  CHECK(lb_store_create_namespace(&f.s, A).code == lb_error_ok);
  put(&f.s, A, 10, "old");
  const uint64_t old_offset = offset(&f.s, A, 10);
  const auto old_value = lb_index_get(lb_directory_get(&f.s.namespaces, A), 10);
  lb_store_request write{}, read{}, second{};
  char bytes[20];
  CHECK(lb_store_prepare_write(&f.s, &write, A, 10, "new", 3, hash("new")).code == lb_error_ok);
  CHECK(lb_index_get(lb_directory_get(&f.s.namespaces, A), 10) == old_value);
  CHECK(lb_store_prepare_read(&f.s, &read, A, 10, bytes, sizeof(bytes)).code == lb_error_busy);
  CHECK(lb_store_clear_namespace(&f.s, A, 256).code == lb_error_busy);
  CHECK(lb_store_remove_namespace(&f.s, A, 256).code == lb_error_busy);
  CHECK(lb_store_destroy(&f.s).code == lb_error_busy);
  const auto before_growth = f.s.namespaces.buckets;
  for (uint64_t i = 100; i < 300; ++i)
    CHECK(lb_store_create_namespace(&f.s, {i, i * 3}).code == lb_error_ok);
  CHECK(f.s.namespaces.buckets > before_growth);
  CHECK(drive(&write, 1).code == lb_error_ok);
  CHECK(get(&f.s, A, 10) == "new");
  put(&f.s, A, 11, "reuse");
  CHECK(offset(&f.s, A, 11) == old_offset);
  CHECK(lb_store_prepare_read(&f.s, &read, A, 10, bytes, sizeof(bytes)).code == lb_error_ok);
  char other[20];
  CHECK(lb_store_prepare_read(&f.s, &second, A, 11, other, sizeof(other)).code == lb_error_ok);
  CHECK(lb_store_prepare_read(&f.s, &write, A, 10, other, sizeof(other)).code ==
        lb_error_pending_full);
  CHECK(lb_store_erase_record(&f.s, A, 10).code == lb_error_busy);
  CHECK(lb_store_prepare_write(&f.s, &write, A, 10, "bad", 3, 0).code == lb_error_busy);
  CHECK(lb_store_cancel(&second).code == lb_error_cancelled);
  lb_store_io_operation op{};
  CHECK(lb_store_start_io(&read, &op).code == lb_error_ok);
  CHECK(lb_store_cancel(&read).code == lb_error_busy);
  CHECK(lb_store_erase_record(&f.s, A, 10).code == lb_error_busy);
  CHECK(lb_store_complete_io(&read, int(pread(op.fd, op.buffer, op.length, op.offset))).code ==
        lb_error_ok);
  CHECK(std::string(bytes, read.transferred) == "new");
  CHECK(lb_store_complete_io(&read, 3).code == lb_error_invalid_state);
  CHECK(f.s.pending_count == 0);
  destroy(&f);
}
void failures_and_reuse() {
  fixture f;
  create(&f, 64);
  CHECK(lb_store_create_namespace(&f.s, A).code == lb_error_ok);
  put(&f.s, A, 1, "old");
  {
    lb_store_request partial{};
    CHECK(lb_store_prepare_write(&f.s, &partial, A, 1, "new", 3, 99).code == lb_error_ok);
    lb_store_io_operation op{};
    CHECK(lb_store_start_io(&partial, &op).code == lb_error_ok);
    CHECK(pwrite(op.fd, op.buffer, 1, op.offset) == 1);
    CHECK(lb_store_complete_io(&partial, 1).code == lb_error_more_io);
    CHECK(lb_store_start_io(&partial, &op).code == lb_error_ok);
    CHECK(op.length == 2);
    CHECK(lb_store_complete_io(&partial, -EIO).code == lb_error_io_failure);
    CHECK(get(&f.s, A, 1) == "old");
  }
  for (int completion : {-EIO, -ENOSPC, -ECANCELED, 0, 99, INT_MIN}) {
    lb_store_request r{};
    CHECK(lb_store_prepare_write(&f.s, &r, A, 1, "new", 3, 99).code == lb_error_ok);
    lb_store_io_operation op{};
    CHECK(lb_store_start_io(&r, &op).code == lb_error_ok);
    CHECK(lb_store_complete_io(&r, completion).code != lb_error_ok);
    CHECK(get(&f.s, A, 1) == "old");
    CHECK(f.s.values.file_allocator.high_watermark == 64);
  }
  lb_store_request r{};
  CHECK(lb_store_prepare_write(&f.s, &r, A, 1, "new", 3, 99).code == lb_error_ok);
  const uint64_t rollback_offset = lb_voffset(r.value);
  CHECK(lb_store_cancel(&r).code == lb_error_cancelled);
  CHECK(lb_store_prepare_write(&f.s, &r, A, 1, "new", 3, hash("new")).code == lb_error_ok);
  CHECK(lb_voffset(r.value) == rollback_offset);
  lb_store_io_operation op{};
  CHECK(lb_store_start_io(&r, &op).code == lb_error_ok);
  CHECK(lb_store_complete_io(&r, -EINTR).code == lb_error_more_io);
  CHECK(lb_store_start_io(&r, &op).code == lb_error_ok);
  CHECK(lb_store_complete_io(&r, -EAGAIN).code == lb_error_more_io);
  CHECK(drive(&r, 1).code == lb_error_ok);
  CHECK(get(&f.s, A, 1) == "new");
  put(&f.s, A, 2, "full");
  CHECK(lb_store_prepare_write(&f.s, &r, A, 1, "newer", 5, 0).code == lb_error_capacity);
  CHECK(get(&f.s, A, 1) == "new");
  CHECK(lb_store_erase_record(&f.s, A, 2).code == lb_error_ok);
  CHECK(lb_store_prepare_write(&f.s, &r, A, 3, "fail index", 10, 0).code == lb_error_ok);
  CHECK(lb_store_cancel(&r).code == lb_error_cancelled);
  CHECK(get(&f.s, A, 1) == "new");
  put(&f.s, A, 3, "retry");
  CHECK(get(&f.s, A, 3) == "retry");
  // Real EOF must fail instead of looping forever or reporting a full value.
  CHECK(ftruncate(f.s.values.file_descriptor, 0) == 0);
  char buffer[10];
  CHECK(lb_store_prepare_read(&f.s, &r, A, 1, buffer, sizeof(buffer)).code == lb_error_ok);
  CHECK(drive(&r).code == lb_error_io_failure);
  destroy(&f);
}
inline bool fail_index_allocation = false;
void *allocate_index(size_t alignment, size_t bytes) {
  if (fail_index_allocation)
    return nullptr;
  return alignment ? std::aligned_alloc(alignment, bytes) : std::malloc(bytes);
}
void index_failure() {
  fixture f;
  int fd = mkstemp(f.path);
  CHECK(fd >= 0);
  close(fd);
  CHECK(lb_store_create(&f.s, f.path, 128, 64, 1, allocate_index, ct_system_allocate).code ==
        lb_error_ok);
  CHECK(lb_store_create_namespace(&f.s, A).code == lb_error_ok);
  lb_store_request r{};
  CHECK(lb_store_prepare_write(&f.s, &r, A, 1, "old", 3, hash("old")).code == lb_error_ok);
  const auto failed_offset = lb_voffset(r.value);
  fail_index_allocation = true;
  CHECK(drive(&r).code == lb_error_no_memory);
  CHECK(lb_store_lookup_namespace(&f.s, A) == lb_presence_empty);
  CHECK(f.s.nodes.stats.live_bytes == 0 && f.s.nodes.stats.reserved_bytes == 0);
  fail_index_allocation = false;
  put(&f.s, A, 1, "old");
  CHECK(offset(&f.s, A, 1) == failed_offset);
  fail_index_allocation = true;
  put(&f.s, A, 1, "new"); // replacement needs no index allocation
  CHECK(get(&f.s, A, 1) == "new");
  fail_index_allocation = false;
  // Populated special-key namespaces must also be reclaimed on store teardown.
  CHECK(lb_store_create_namespace(&f.s, lb_namespace{UINT64_MAX, UINT64_MAX}).code == lb_error_ok);
  put(&f.s, lb_namespace{UINT64_MAX, UINT64_MAX}, UINT64_MAX, "special");
  CHECK(lb_store_destroy(&f.s).code == lb_error_ok);
  CHECK(f.s.nodes.stats.live_bytes == 0 && f.s.nodes.stats.reserved_bytes == 0);
  destroy(&f);
}
void deletion_failure() {
  fixture f;
  int fd = mkstemp(f.path);
  CHECK(fd >= 0);
  close(fd);
  CHECK(
      lb_store_create(&f.s, f.path, 1024 * 1024, 64, 1, allocate_index, ct_system_allocate).code ==
      lb_error_ok);
  CHECK(lb_store_create_namespace(&f.s, A).code == lb_error_ok);
  std::vector<uint64_t> keys;
  for (size_t i = 0; i < 512; ++i) {
    keys.push_back(lb_key_hash(i));
    put(&f.s, A, keys.back(), "old");
  }
  bool failed = false;
  for (auto key : keys) {
    // Occupy the pool's spare blocks and deny new system allocations. This
    // forces a joining deletion to fail without depending on slab occupancy.
    std::vector<std::pair<void *, size_t>> held;
    fail_index_allocation = true;
    for (size_t cap = 4; 24 + 8 * cap <= 4096; cap += std::max<size_t>(1, cap / 4)) {
      const auto bytes = 24 + 8 * cap;
      while (auto *block = ct_pool_allocate(&f.s.nodes, bytes))
        held.emplace_back(block, bytes);
    }
    auto *index = lb_directory_get(&f.s.namespaces, A);
    auto tree = lb_index_view(index);
    std::vector<ct_record> before(ct_node_count(index->root));
    CHECK(ct_tree_list(&tree, {}, before.data(), before.size()) == before.size());
    const auto erased = lb_store_erase_record(&f.s, A, key);
    for (auto [block, bytes] : held)
      ct_pool_deallocate(&f.s.nodes, block, bytes);
    fail_index_allocation = false;
    if (erased.code == lb_error_no_memory) {
      tree = lb_index_view(index);
      CHECK(ct_tree_validate(&tree) && ct_node_count(index->root) == before.size());
      for (size_t i = 0; i < before.size(); ++i) {
        ct_record actual{};
        CHECK(lb_index_select(index, i, &actual));
        CHECK(actual.key == before[i].key && actual.value == before[i].value &&
              actual.hash == before[i].hash);
      }
      CHECK(get(&f.s, A, key) == "old");
      CHECK(lb_store_erase_record(&f.s, A, key).code == lb_error_ok);
      put(&f.s, A, key, "new");
      CHECK(get(&f.s, A, key) == "new");
      failed = true;
      break;
    }
    CHECK(erased.code == lb_error_ok);
  }
  CHECK(failed);
  destroy(&f);
}
void metadata_failure() {
  fixture f;
  create(&f);
  CHECK(lb_store_create_namespace(&f.s, A).code == lb_error_ok);
  put(&f.s, A, 1, "old");
  auto &metadata = f.s.values.file_allocator.slab_allocator;
  const auto saved = metadata;
  metadata.high_watermark_index = metadata.size = INT_MAX;
  metadata.free_slab_head.next_index = -1;
  CHECK(lb_store_erase_record(&f.s, A, 1).code == lb_error_no_memory);
  lb_store_request r{};
  CHECK(lb_store_prepare_write(&f.s, &r, A, 1, "new", 3, 0).code == lb_error_no_memory);
  CHECK(lb_store_clear_namespace(&f.s, A, 256).code == lb_error_no_memory);
  CHECK(get(&f.s, A, 1) == "old");
  metadata = saved;
  // Tickets acquired before submission make failed-I/O cleanup allocation-free.
  CHECK(lb_store_prepare_write(&f.s, &r, A, 1, "new", 3, 0).code == lb_error_ok);
  const int saved_size = metadata.size;
  const int saved_high = metadata.high_watermark_index;
  metadata.high_watermark_index = metadata.size = INT_MAX;
  lb_store_io_operation op{};
  CHECK(lb_store_start_io(&r, &op).code == lb_error_ok);
  CHECK(lb_store_complete_io(&r, -ENOSPC).code == lb_error_io_failure);
  metadata.size = saved_size;
  metadata.high_watermark_index = saved_high;
  CHECK(get(&f.s, A, 1) == "old");
  // Successful retirement must also work when no new metadata can be allocated.
  CHECK(lb_store_prepare_write(&f.s, &r, A, 1, "new", 3, hash("new")).code == lb_error_ok);
  const int success_size = metadata.size, success_high = metadata.high_watermark_index;
  metadata.high_watermark_index = metadata.size = INT_MAX;
  CHECK(drive(&r).code == lb_error_ok);
  metadata.size = success_size;
  metadata.high_watermark_index = success_high;
  CHECK(get(&f.s, A, 1) == "new");
  put(&f.s, A, 1, "retry");
  destroy(&f);
}
inline size_t namespace_allocations = 0, fail_namespace_at = 0;
void *allocate_namespace(size_t alignment, size_t bytes) {
  if (++namespace_allocations == fail_namespace_at)
    return nullptr;
  return std::aligned_alloc(alignment, bytes);
}
void namespace_failures_and_relocation() {
  fixture f;
  create(&f);
  CHECK(lb_store_destroy(&f.s).code == lb_error_ok);
  for (size_t failure : {size_t(1), size_t(2)}) {
    namespace_allocations = 0;
    fail_namespace_at = failure;
    CHECK(lb_store_create(&f.s, f.path, 1024, 64, 16, nullptr, allocate_namespace).code ==
          lb_error_no_memory);
    CHECK(!f.s.opened && !f.s.namespaces.initialized);
    CHECK(lb_store_destroy(&f.s).code == lb_error_ok);
  }
  fail_namespace_at = 0;
  CHECK(lb_store_create(&f.s, f.path, 1024, 64, 0, nullptr, allocate_namespace).code ==
        lb_error_ok);
  CHECK(lb_store_create_namespace(&f.s, B).code == lb_error_ok);
  put(&f.s, B, 20, "remove");
  CHECK(lb_store_create_namespace(&f.s, A).code == lb_error_ok);
  put(&f.s, A, 10, "old");
  lb_store_request write{};
  CHECK(lb_store_prepare_write(&f.s, &write, A, 10, "new", 3, hash("new")).code == lb_error_ok);
  // Erasing the first dense entry moves the last header, whose I/O is pending.
  CHECK(lb_store_remove_namespace(&f.s, B, 256).code == lb_error_ok);
  CHECK(lb_namespace_equal(f.s.namespaces.entries[0].key, A));
  size_t failed_growths = 0;
  for (uint64_t i = 100; i < 300; ++i) {
    const auto count = f.s.namespaces.count;
    fail_namespace_at = namespace_allocations + 1;
    auto outcome = lb_store_create_namespace(&f.s, {i, i * 3});
    if (outcome.code != lb_error_ok) {
      CHECK(outcome.code == lb_error_no_memory);
      ++failed_growths;
      CHECK(f.s.namespaces.count == count);
      CHECK(lb_store_lookup_namespace(&f.s, {i, i * 3}) == lb_presence_absent);
      CHECK(ct_node_hash(lb_directory_get(&f.s.namespaces, A)->root) == hash("old"));
      fail_namespace_at = 0;
      CHECK(lb_store_create_namespace(&f.s, {i, i * 3}).code == lb_error_ok);
    }
  }
  CHECK(failed_growths > 5);
  fail_namespace_at = 0;
  CHECK(drive(&write).code == lb_error_ok);
  CHECK(get(&f.s, A, 10) == "new");
  destroy(&f);
}
void reset_and_lock() {
  fixture f;
  create(&f);
  CHECK(lb_store_create_namespace(&f.s, A).code == lb_error_ok);
  put(&f.s, A, 1, "cached");
  lb_store other{};
  CHECK(lb_store_create(&other, f.path, 1024, 64, 0, NULL, ct_system_allocate).code ==
        lb_error_io_failure);
  CHECK(get(&f.s, A, 1) == "cached");
  CHECK(lb_store_destroy(&other).code == lb_error_ok);
  CHECK(lb_store_create(&other, f.path, 0, 64, 0, NULL, ct_system_allocate).code ==
        lb_error_invalid_argument);
  CHECK(lb_store_create(&other, f.path, LB_OFFSET_MASK + 2, 64, 0, NULL, ct_system_allocate).code ==
        lb_error_invalid_argument);
  CHECK(lb_store_destroy(&f.s).code == lb_error_ok);
  CHECK(lb_store_create(&f.s, f.path, 1024, 64, 0, NULL, ct_system_allocate).code == lb_error_ok);
  struct stat info{};
  CHECK(fstat(f.s.values.file_descriptor, &info) == 0 && info.st_size == 0);
  CHECK(lb_store_lookup_namespace(&f.s, A) == lb_presence_absent);
  CHECK(f.s.values.file_allocator.high_watermark == 0);
  destroy(&f);
}
void concurrent_records() {
  fixture f;
  create(&f);
  CHECK(lb_store_create_namespace(&f.s, A).code == lb_error_ok);
  lb_store_request writes[8]{};
  for (size_t i = 0; i < 8; ++i)
    CHECK(lb_store_prepare_write(&f.s, &writes[i], A, i, "batch", 5, hash("batch")).code ==
          lb_error_ok);
  CHECK(lb_store_clear_namespace(&f.s, A, 256).code == lb_error_busy);
  CHECK(lb_store_lookup_namespace(&f.s, A) == lb_presence_empty);
  // Remove middle, head and tail requests, keeping all other conflicts live.
  bool finished[8]{};
  size_t completed = 0;
  for (size_t i : {3, 7, 0, 5, 2, 6, 1, 4}) {
    CHECK(drive(&writes[i]).code == lb_error_ok);
    finished[i] = true;
    CHECK(f.s.pending_count == 8 - ++completed);
    for (size_t j = 0; j < 8; ++j)
      if (!finished[j]) {
        lb_store_request conflict{};
        CHECK(lb_store_prepare_write(&f.s, &conflict, A, j, "bad", 3, 0).code == lb_error_busy);
      }
  }
  CHECK(!f.s.pending);
  for (size_t i = 0; i < 8; ++i)
    CHECK(get(&f.s, A, i) == "batch");
  lb_store_request reads[2]{};
  char buffers[2][5];
  for (size_t i = 0; i < 2; ++i)
    CHECK(lb_store_prepare_read(&f.s, &reads[i], A, 1, buffers[i], 5).code == lb_error_ok);
  CHECK(lb_store_erase_record(&f.s, A, 2).code == lb_error_ok);
  CHECK(drive(&reads[1]).code == lb_error_ok);
  CHECK(lb_store_erase_record(&f.s, A, 1).code == lb_error_busy);
  CHECK(drive(&reads[0]).code == lb_error_ok);
  CHECK(lb_store_erase_record(&f.s, A, 1).code == lb_error_ok);
  destroy(&f);
}
void randomized() {
  fixture f;
  create(&f, 4 * 1024 * 1024);
  std::map<uint64_t, std::string> expected;
  std::mt19937_64 random(0xCA4E);
  CHECK(lb_store_create_namespace(&f.s, A).code == lb_error_ok);
  for (size_t step = 0; step < 5000; ++step) {
    const uint64_t key = random() % 128;
    if (random() % 3 == 0) {
      auto erased = lb_store_erase_record(&f.s, A, key);
      CHECK(erased.code == (expected.erase(key) ? lb_error_ok : lb_error_record_missing));
    } else {
      std::string bytes(1 + random() % 127, char(random()));
      put(&f.s, A, key, bytes);
      expected[key] = bytes;
    }
    uint64_t fingerprint = 0;
    for (auto &[k, bytes] : expected)
      fingerprint ^= hash(bytes);
    auto *index = lb_directory_get(&f.s.namespaces, A);
    CHECK(ct_node_count(index->root) == expected.size());
    auto tree = lb_index_view(index);
    CHECK(ct_tree_validate(&tree));
    CHECK(ct_node_hash(index->root) == fingerprint);
    uint64_t range = 0;
    for (auto &[k, bytes] : expected)
      if (k >= 32 && k < 96)
        range ^= hash(bytes);
    CHECK(lb_index_summary(index, {32, 96, true}).hash == range);
    if (step % 97 == 0)
      for (auto &[k, bytes] : expected)
        CHECK(get(&f.s, A, k) == bytes);
  }
  while (lb_store_remove_namespace(&f.s, A, 7).code == lb_error_more_work) {
  }
  CHECK(lb_store_lookup_namespace(&f.s, A) == lb_presence_absent);
  destroy(&f);
}
void uring() {
#ifdef __linux__
  io_uring ring{};
  const int initialized = io_uring_queue_init(2, &ring, 0);
  if (initialized < 0) {
    CHECK(std::getenv("LITTLEBEAR_REQUIRE_URING") == nullptr);
    CHECK(initialized == -EPERM || initialized == -ENOSYS || initialized == -EACCES);
    std::printf("SKIP: kernel io_uring unavailable (%d); completion-state tests still run\n",
                initialized);
    return;
  }
  fixture f;
  create(&f);
  CHECK(lb_store_create_namespace(&f.s, A).code == lb_error_ok);
  CHECK(lb_store_create_namespace(&f.s, B).code == lb_error_ok);
  lb_store_request write{}, second{}, read{};
  CHECK(lb_store_prepare_write(&f.s, &write, A, 1, "uring", 5, 1).code == lb_error_ok);
  CHECK(lb_store_prepare_write(&f.s, &second, A, 2, "other", 5, 2).code == lb_error_ok);
  // Fill the SQ before enqueueing: request stays ready and owns its buffer.
  for (size_t i = 0; i < 2; ++i) {
    auto *sqe = io_uring_get_sqe(&ring);
    CHECK(sqe != nullptr);
    io_uring_prep_nop(sqe);
  }
  CHECK(!lb_uring_enqueue(&ring, &write, NULL));
  CHECK(write.state == lb_phase_ready);
  CHECK(io_uring_submit(&ring) == 2);
  for (size_t i = 0; i < 2; ++i) {
    io_uring_cqe *cqe;
    CHECK(io_uring_wait_cqe(&ring, &cqe) == 0);
    CHECK(cqe->res == 0);
    io_uring_cqe_seen(&ring, cqe);
  }
  CHECK(lb_uring_enqueue(&ring, &write, NULL));
  CHECK(lb_uring_enqueue(&ring, &second, NULL));
  CHECK(!lb_uring_enqueue(&ring, &write, NULL));
  CHECK(io_uring_submit(&ring) == 2);
  for (size_t i = 0; i < 2; ++i) {
    io_uring_cqe *cqe;
    CHECK(io_uring_wait_cqe(&ring, &cqe) == 0);
    CHECK(lb_uring_complete(cqe).code == lb_error_ok);
    io_uring_cqe_seen(&ring, cqe);
  }
  char buffer[5];
  CHECK(lb_store_prepare_read(&f.s, &read, A, 1, buffer, sizeof(buffer)).code == lb_error_ok);
  CHECK(lb_uring_enqueue(&ring, &read, NULL));
  CHECK(io_uring_submit(&ring) == 1);
  io_uring_cqe *cqe;
  CHECK(io_uring_wait_cqe(&ring, &cqe) == 0);
  CHECK(lb_uring_complete(cqe).code == lb_error_ok);
  io_uring_cqe_seen(&ring, cqe);
  CHECK(std::string(buffer, 5) == "uring");
  CHECK(get(&f.s, A, 2) == "other");
  io_uring_queue_exit(&ring);
  destroy(&f);
  std::puts("PASS: real io_uring batched writes, read, and SQ exhaustion");
#endif
}
} // namespace storage_tests
int main() {
  storage_tests::lifecycle();
  storage_tests::pending_and_replacement();
  storage_tests::failures_and_reuse();
  storage_tests::metadata_failure();
  storage_tests::index_failure();
  storage_tests::deletion_failure();
  storage_tests::namespace_failures_and_relocation();
  storage_tests::reset_and_lock();
  storage_tests::concurrent_records();
  storage_tests::randomized();
  storage_tests::uring();
  std::printf("Storage metadata: store=%zu bytes, request=%zu bytes (64 requests=%zu bytes, "
              "excluding buffers)\n",
              sizeof(lb_store), sizeof(lb_store_request), 64 * sizeof(lb_store_request));
  std::puts("PASS: namespace/record lifecycle, short I/O, rollback, cancellation, pinning, reset, "
            "and 5,000 model operations");
}
