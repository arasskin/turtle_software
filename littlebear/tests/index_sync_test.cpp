#include "check.h"
#include "index_sync.h"
#include <set>
#include <vector>

int main() {
  ct_pool nodes{};
  ct_pool_init(&nodes, nullptr);
  auto a = lb_index_create(&nodes), b = lb_index_create(&nodes);
  std::set<uint64_t> expected_have, expected_need, got_have, got_need;
  for (uint64_t i = 1; i < 4000; ++i) {
    const uint64_t id = i, hash = lb_key_hash(i);
    const bool changed = i >= 1500 && i < 1600;
    if (i < 3000)
      CHECK(lb_index_set(&a, id, 1, hash) == 1);
    if (i >= 1000)
      CHECK(lb_index_set(&b, id, 1, changed ? lb_key_hash(i + 4000) : hash) == 1);
    if (i < 1000 || changed)
      expected_have.insert(id);
    if (i >= 3000 || changed)
      expected_need.insert(id);
  }
  using packet = std::vector<uint8_t>;
  lb_recon_workspace work{};
  uint8_t memory[4096];
  lb_recon_output out{memory, sizeof(memory), 0, 0};
  CHECK(lb_recon_begin(lb_index_source(&a), &work, &out) == lb_recon_status_complete);
  std::vector<packet> packets{{memory, memory + out.size}};
  bool to_b = true;
  size_t turns = 0;
  while (!packets.empty()) {
    CHECK(++turns < 100);
    std::vector<packet> next;
    for (auto &input : packets) {
      lb_recon_cursor cursor{};
      uint64_t have[32], need[32];
      lb_recon_differences differences{have, 32, 0, need, 32, 0};
      for (;;) {
        const auto code =
            lb_recon_receive(lb_index_source(to_b ? &b : &a),
                             to_b ? lb_recon_role_responder : lb_recon_role_initiator,
                             {input.data(), input.size()}, &cursor, &work, &out, &differences, 3);
        CHECK(code == lb_recon_status_complete || code == lb_recon_status_more ||
              code == lb_recon_status_output_full || code == lb_recon_status_differences_full);
        if (!to_b) {
          got_have.insert(have, have + differences.have_count);
          got_need.insert(need, need + differences.need_count);
        }
        if (out.size > sizeof(lb_recon_MAGIC))
          next.emplace_back(memory, memory + out.size);
        if (code == lb_recon_status_complete)
          break;
      }
    }
    packets = std::move(next);
    to_b = !to_b;
  }
  CHECK(got_have == expected_have && got_need == expected_need);
  auto av = lb_index_view(&a), bv = lb_index_view(&b);
  CHECK(ct_tree_validate(&av) && ct_tree_validate(&bv));
  lb_index_destroy(&a);
  lb_index_destroy(&b);
  ct_pool_destroy(&nodes);
  std::puts("PASS: key-ordered wide-tree reconciliation with independent hashes and 100 same-key "
            "content edits");
}
