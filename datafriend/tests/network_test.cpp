#include "../../littlebear/tests/cache_driver.h"
#include "test_support.h"

std::string handle(lb_test_cache *server, unsigned op, std::string_view name, bool writable,
                   uint64_t epoch, uint64_t revision, std::string_view payload) {
  lb_buffer out{};
  require(lb_test_handle(server, op, lb_name(reinterpret_cast<const uint8_t *>(name.data())), writable,
                    epoch, revision, lb_bytes_make(payload.data(), payload.size()), &out),
          "server request");
  auto packet = text(lb_bytes_make(out.data, out.size));
  lb_buffer_free(&out);
  return packet;
}
std::string opening(lb_test_cache *server, std::string_view name) {
  lb_buffer out{};
  auto *entry = lb_test_cache_entry(server, lb_name(reinterpret_cast<const uint8_t *>(name.data())));
  bool busy = entry && entry->writing;
  lb_reply_header(&out, busy ? 1 : entry ? 0 : 3, server->epoch, entry ? entry->revision : 0);
  if (entry && !busy)
    require(lb_exchange_begin(lb_index_source(&entry->value), &out), "server opening");
  require(!out.failed, "server opening allocation");
  auto packet = text(lb_bytes_make(out.data, out.size));
  lb_buffer_free(&out);
  return packet;
}
std::optional<std::string> delta_packet(df_store *db, uint64_t after) {
  df_buffer out{};
  int rc = df_delta(db, after, &out);
  auto packet = text(lb_bytes_make(out.data, out.size));
  df_free(out.data);
  if (rc == 2)
    return {};
  require(rc == 0, "delta");
  return packet;
}
std::optional<std::vector<uint64_t>> apply_delta(df_store *db, std::string_view packet) {
  df_buffer out{};
  int rc =
      df_apply_delta(db, reinterpret_cast<const uint8_t *>(packet.data()), packet.size(), &out);
  auto reply = text(lb_bytes_make(out.data, out.size));
  df_free(out.data);
  if (rc == 2)
    return {};
  require(rc == 0, "apply delta");
  return changes(reply);
}
struct exchange {
  lb_test_cache &server;
  uint64_t epoch = 0, revision = 0;
  bool absent = false;
  std::string name = std::string(16, 'a');
  std::string request(unsigned op, std::string_view payload = {}, bool writable = true) {
    auto packet = handle(&server, op, name, writable, epoch, revision, payload);
    lb_reader r{span(packet), false};
    auto status = lb_read_u8(&r);
    epoch = lb_read_u64(&r);
    revision = lb_read_u64(&r);
    if (op == 0 && status == 3) {
      absent = true;
      return text(r.data);
    }
    if (op == 0)
      absent = false;
    if (status)
      throw std::runtime_error("stale");
    return text(r.data);
  }
  void reconcile(df_store *db, bool server_starts = false) {
    std::string packet;
    if (server_starts) {
      auto opening = ::opening(&server, name);
      lb_reader r{span(opening), false};
      require(lb_read_u8(&r) == 0, "server opening failed");
      epoch = lb_read_u64(&r);
      revision = lb_read_u64(&r);
      packet = sync(db, 5, text(r.data));
      if (packet.size() == 4)
        return;
    } else {
      packet = sync(db, 1, request(0, sync(db, 0)));
      if (packet.size() == 4)
        return;
    }
    for (unsigned i = 0;; ++i) {
      require(i < 128, "round limit");
      packet = sync(db, 1, request(1, packet));
      if (packet.size() == 4)
        break;
    }
  }
  void push(df_store *db) {
    reconcile(db);
    for (;;) {
      auto batch = sync(db, 2);
      lb_reader r{span(batch), false};
      if (!lb_read_u32(&r) && !absent)
        break;
      request(3, batch);
      absent = false;
    }
    sync(db, 4);
  }
  std::string fetch(df_store *db) {
    auto keys = sync(db, 3);
    return request(2, std::string_view(keys).substr(1));
  }
  auto pull(df_store *db) {
    reconcile(db, true);
    std::vector<uint64_t> changes;
    for (;;) {
      auto keys = sync(db, 3);
      changes = sync_apply(db, request(2, std::string_view(keys).substr(1)));
      if (keys[0])
        break;
    }
    sync(db, 4);
    return changes;
  }
};
int main() {
  directory root;
  auto owner = open_store(root.path + "/owner"), replica = open_store(root.path + "/replica");
  lb_test_cache server{};
  require(lb_test_cache_create(&server, (root.path + "/cache").c_str(), 77, 1 << 26).code ==
              lb_error_ok,
          "server create");
  exchange x{server};
  auto all = df_watch_query(replica, {nullptr, 0, true}, nullptr, nullptr);
  require(all, "watch");
  auto handle = all->result;
  std::vector<fact> initial;
  for (uint32_t i = 0; i < 4000; ++i) {
    initial.push_back({i, DF_ATTR_TITLE, "task" + std::to_string(i)});
    initial.push_back({i, DF_ATTR_DONE, "false"});
  }
  initial.push_back({UINT32_MAX, DF_ATTR_TITLE, "last"});
  edit_store(owner, std::move(initial));
  x.push(owner);
  auto changed = x.pull(replica);
  require(changed == std::vector<uint64_t>{all->id}, "pull notification");
  require(export_records(owner) == export_records(replica) && df_view_size(handle) == 8001,
          "initial convergence");
  require(x.pull(replica).empty(), "equal pull notified");
  edit_store(owner, {{5, DF_ATTR_TITLE, "changed"}, {7000, DF_ATTR_DONE, "true"}},
             {{3, DF_ATTR_TITLE, "task3"}});
  x.push(owner);
  x.pull(replica);
  require(export_records(owner) == export_records(replica), "edit/delete convergence");
  // Stable snapshot survives local edits; a later exchange publishes the latest.
  x.reconcile(owner);
  auto before = export_records(owner);
  edit_store(owner, {{5, DF_ATTR_TITLE, "during exchange"}}, {});
  x.request(3, sync(owner, 2));
  sync(owner, 4);
  x.pull(replica);
  require(export_records(replica) == before, "snapshot changed during exchange");
  x.push(owner);
  x.pull(replica);
  // Pull must not overwrite a local transaction that happened during the exchange.
  x.reconcile(replica);
  edit_store(replica, {{0, DF_ATTR_TITLE, "local concurrent edit"}}, {});
  bool stale = false;
  try {
    sync_apply(replica, x.fetch(replica));
  } catch (...) {
    stale = true;
  }
  require(stale, "stale pull accepted");
  sync(replica, 4);
  x.pull(replica);
  require(export_records(owner) == export_records(replica), "retry convergence");
  exchange old{server};
  old.reconcile(replica);
  edit_store(owner, {{4, DF_ATTR_DONE, "true"}}, {});
  x.push(owner);
  stale = false;
  try {
    old.fetch(replica);
  } catch (...) {
    stale = true;
  }
  require(stale, "stale server version accepted");
  sync(replica, 4);
  // Authentication's write decision reaches storage before mutation.
  bool rejected = false;
  lb_buffer empty{};
  lb_u32(&empty, 0);
  try {
    x.request(3, text(lb_buffer_bytes(&empty)), false);
  } catch (...) {
    rejected = true;
  }
  require(rejected, "read-only write accepted");
  // Malformed entire batch is rejected before a valid earlier record is changed.
  lb_buffer bad{};
  lb_u32(&bad, 2);
  auto facts = export_records(owner);
  auto [key, record] = *facts.begin();
  lb_u64(&bad, key);
  lb_u8(&bad, 0);
  lb_u64(&bad, key + 1);
  lb_u8(&bad, 1);
  lb_blob(&bad, span("bad"));
  rejected = false;
  try {
    x.request(3, text(lb_buffer_bytes(&bad)));
  } catch (...) {
    rejected = true;
  }
  require(rejected, "malformed batch accepted");
  x.pull(replica);
  require(export_records(owner) == export_records(replica), "malformed batch partially applied");
  // Namespace separation, empty-owner deletions, and uint32 endpoint IDs.
  x.name = std::string(16, 'b');
  auto emptyOwner = open_store(root.path + "/empty");
  x.push(emptyOwner);
  x.pull(replica);
  require(export_records(replica).empty(), "remote authoritative deletion");
  x.name = std::string(16, 'a');
  x.pull(replica);
  require(export_records(owner) == export_records(replica), "namespace isolation");
  // Every truncation fails without importing anything.
  x.reconcile(replica);
  auto records = x.fetch(replica);
  for (size_t n = 0; n < records.size(); ++n) {
    rejected = false;
    try {
      sync_apply(replica, std::string_view(records).substr(0, n));
    } catch (...) {
      rejected = true;
    }
    require(rejected, "truncated import accepted");
  }
  sync(replica, 4);
  // Prepared async writes own their buffers, block conflicting namespace
  // exchanges, and can be cancelled before submission without changing facts.
  lb_buffer asyncBatch{};
  lb_u32(&asyncBatch, 1);
  lb_u64(&asyncBatch, pair_key(9000, 1));
  lb_u8(&asyncBatch, 1);
  lb_blob(&asyncBatch, span(fact_record(pair_key(9000, 1), "async")));
  lb_task task{};
  require(lb_test_task_prepare(&task, 3, lb_name(reinterpret_cast<const uint8_t *>(x.name.data())),
                       true, x.epoch, x.revision, lb_bytes_make(asyncBatch.data, asyncBatch.size)),
          "task init");
  lb_test_task_step(&task, &server);
  require(task.pending.first.owner && task.pending.first.state == lb_phase_ready && !(task.phase == lb_task_done),
          "async write did not suspend");
  auto busy = ::handle(&server, 0, std::string(16, 'a'), true, 0, 0, std::string(1, 0));
  require(uint8_t(busy[0]) == 1, "concurrent request observed transferring namespace");
  lb_test_task_dispose(&task, &server); // cancels a prepared request and releases the namespace guard
  x.pull(replica);
  require(export_records(owner) == export_records(replica), "cancelled async write published");
  // A subscriber pins one file read, not its whole namespace. An owner can
  // publish another record while that read is outstanding; the old fetch then
  // returns a fresh opening instead of publishing its stale bytes.
  lb_buffer fetchOne{};
  lb_u32(&fetchOne, 1);
  lb_u64(&fetchOne, export_records(owner).begin()->first);
  lb_task reading{};
  require(lb_test_task_prepare(&reading, 2,
                       lb_name(reinterpret_cast<const uint8_t *>(x.name.data())), false, x.epoch,
                       x.revision, lb_bytes_make(fetchOne.data, fetchOne.size)),
          "task init");
  lb_test_task_step(&reading, &server);
  require(reading.pending.first.owner && reading.pending.first.state == lb_phase_ready, "fetch did not suspend");
  lb_store_io_operation readIO{};
  require(lb_store_start_io(&reading.pending.first, &readIO).code == lb_error_ok, "start pinned read");
  auto readBytes = pread(readIO.fd, readIO.buffer, readIO.length, readIO.offset);
  edit_store(owner, {{9001, DF_ATTR_TITLE, "owner continues during subscriber fetch"}}, {});
  x.push(owner);
  lb_store_complete_io(&reading.pending.first, readBytes < 0 ? -errno : int(readBytes));
  lb_test_task_step(&reading, &server);
  require((reading.phase == lb_task_done), "stale read did not finish");
  auto restart = text(lb_bytes_make(reading.reply.data, reading.reply.size));
  lb_reader restartReader{span(restart), false};
  require(lb_read_u8(&restartReader) == 1, "stale read did not restart");
  lb_read_u64(&restartReader);
  lb_read_u64(&restartReader);
  require(text(restartReader.data).starts_with("LBR\3"), "restart omitted new opening");
  lb_test_task_dispose(&reading, &server);
  x.pull(replica);
  require(export_records(owner) == export_records(replica), "subscriber restart convergence");
  // Complete a write through short kernel-style completions, then read it.
  require(lb_test_task_prepare(&task, 3, lb_name(reinterpret_cast<const uint8_t *>(x.name.data())),
                       true, x.epoch, x.revision, lb_bytes_make(asyncBatch.data, asyncBatch.size)),
          "task init");
  lb_test_task_step(&task, &server);
  while (!(task.phase == lb_task_done)) {
    auto *pending = &task.pending.first;
    require(pending != nullptr, "missing pending request");
    while (pending->state == lb_phase_ready) {
      lb_store_io_operation operation{};
      require(lb_store_start_io(pending, &operation).code == lb_error_ok, "start async I/O");
      auto n = pwrite(operation.fd, operation.buffer, std::min(operation.length, uint32_t(3)),
                      operation.offset);
      lb_store_complete_io(pending, n < 0 ? -errno : int(n));
    }
    lb_test_task_step(&task, &server);
  }
  require(task.reply.data[0] == 0, "async completion failed");
  lb_test_task_dispose(&task, &server);
  x.push(owner); // removes the test-only record before the large transfer
  // Transfer larger than a wire frame: pages must publish exactly once.
  std::vector<fact> large;
  for (uint32_t i = 10000; i < 10160; ++i)
    large.push_back({i, DF_ATTR_TITLE, std::string(60000, 'x')});
  edit_store(owner, std::move(large));
  x.push(owner);
  require(x.pull(replica) == std::vector<uint64_t>{all->id}, "paged import notification");
  require(export_records(owner) == export_records(replica), "large paged transfer");
  // Optimistic batches are paired with the same stable snapshot fingerprint.
  // Subscriber mismatch must leave durable data, indexes and live views intact.
  x.name = std::string(16, 'c');
  auto fast = open_store(root.path + "/fast"), follower = open_store(root.path + "/follower");
  df_track(fast);
  x.push(fast);
  x.pull(follower);
  auto watch = df_watch_query(follower, {nullptr, 0, true}, nullptr, nullptr);
  require(watch, "watch");
  auto view = watch->result;
  auto baseline = df_revision(fast);
  edit_store(fast, {{1, DF_ATTR_TITLE, "first"}, {2, DF_ATTR_DONE, "false"}}, {});
  auto delta = delta_packet(fast, baseline);
  require(bool(delta), "missing optimistic batch");
  x.request(6, *delta);
  sync(fast, 4);
  auto published = apply_delta(follower, *delta);
  require(published && *published == std::vector<uint64_t>{watch->id} && df_view_size(view) == 2,
          "optimistic publication");
  require(apply_delta(follower, *delta)->empty(), "duplicate delta notified");
  baseline = df_revision(fast);
  edit_store(fast, {{1, DF_ATTR_TITLE, "second"}}, {{2, DF_ATTR_DONE, "false"}});
  delta = delta_packet(fast, baseline);
  edit_store(follower, {{3, DF_ATTR_TITLE, "extra"}}, {});
  auto unchanged = export_records(follower);
  auto rev = df_revision(follower);
  require(!apply_delta(follower, *delta), "mismatch accepted");
  require(export_records(follower) == unchanged && df_revision(follower) == rev &&
              df_view_size(view) == 3,
          "mismatch partially published");
  x.request(6, *delta);
  sync(fast, 4);
  x.pull(follower);
  require(export_records(follower) == export_records(fast), "subscriber mismatch recovery");
  // Missed server record: applying only the new frame cannot repair the missing
  // prior fact, so continue reconciliation against the retained owner snapshot.
  auto ns = lb_name(reinterpret_cast<const uint8_t *>(x.name.data()));
  require(lb_store_erase_record(&server.cache, ns, pair_key(1, DF_ATTR_TITLE)).code == lb_error_ok,
          "erase server record");
  baseline = df_revision(fast);
  edit_store(fast, {{2, DF_ATTR_DONE, "true"}}, {});
  delta = delta_packet(fast, baseline);
  auto mismatch = ::handle(&server, 6, x.name, true, x.epoch, x.revision, *delta);
  lb_reader mr{span(mismatch), false};
  require(lb_read_u8(&mr) == 1, "server fingerprint mismatch not detected");
  x.epoch = lb_read_u64(&mr);
  x.revision = lb_read_u64(&mr);
  auto continuation = sync(fast, 1, text(mr.data));
  while (continuation.size() != 4)
    continuation = sync(fast, 1, x.request(1, continuation));
  // An intervening local edit must not change that retained snapshot.
  auto snapshot = export_records(fast);
  edit_store(fast, {{4, DF_ATTR_TITLE, "during fallback"}}, {});
  for (;;) {
    auto batch = sync(fast, 2);
    lb_reader r{span(batch), false};
    if (!lb_read_u32(&r))
      break;
    x.request(3, batch);
  }
  sync(fast, 4);
  x.pull(follower);
  require(export_records(follower) == snapshot, "optimistic fallback snapshot changed");
  auto next = delta_packet(fast, baseline + 1);
  require(bool(next), "pending edit lost");
  x.request(6, *next);
  sync(fast, 4);
  x.pull(follower);
  require(export_records(fast) == export_records(follower), "following delta mismatch");
  // Bounded journal drops to ordinary reconciliation, never loses an edit.
  baseline = df_revision(fast);
  std::vector<fact> churn;
  for (unsigned i = 100; i < 4300; ++i)
    churn.push_back({i, DF_ATTR_TITLE, "x"});
  edit_store(fast, std::move(churn));
  require(!delta_packet(fast, baseline), "journal cap did not fall back");
  x.push(fast);
  x.pull(follower);
  require(export_records(fast) == export_records(follower), "journal overflow convergence");
  lb_buffer_free(&empty);
  lb_buffer_free(&bad);
  lb_buffer_free(&asyncBatch);
  lb_buffer_free(&fetchOne);
  lb_test_cache_destroy(&server);
  close_store(owner);
  close_store(replica);
  close_store(emptyOwner);
  close_store(fast);
  close_store(follower);
  std::cout << "PASS: AELMDB/Littlebear reconciliation, snapshots, versions, imports, "
               "notifications and namespaces\n";
}
