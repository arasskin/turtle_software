// Real production-server interoperability; pass numeric host and port explicitly.
#include "datafriend/network.h"
#include "test_support.h"
#include <filesystem>
#include <iostream>
#include <thread>
#include <unistd.h>

int main(int argc, char **argv) {
  try {
    if (argc != 3)
      throw std::runtime_error("usage: transport_test HOST PORT");
    auto root =
        std::filesystem::temp_directory_path() / ("df-transport-" + std::to_string(getpid()));
    std::filesystem::remove_all(root);
    struct cleanup {
      std::filesystem::path p;
      ~cleanup() { std::filesystem::remove_all(p); }
    } guard{root};
    df_buffer error{};
    void *a = df_identity_open(nullptr, 0, 1, &error);
    require(a, "first identity");
    df_free(error.data);
    void *b = df_identity_open(nullptr, 0, 1, &error);
    require(b, "second identity");
    df_free(error.data);
    auto hex = [](const uint8_t *key) {
      std::string s;
      for (unsigned i = 0; i < 32; ++i) {
        s += "0123456789abcdef"[key[i] >> 4];
        s += "0123456789abcdef"[key[i] & 15];
      }
      return s;
    };
    auto ka = hex(df_identity_public(a)), kb = hex(df_identity_public(b));
    auto owner = open_store((root / "owner").string(), DF_ASYNC, 1 << 26, ka.c_str()),
         replica = open_store((root / "replica").string(), DF_ASYNC, 1 << 26, ka.c_str());
    auto other = open_store((root / "other").string(), DF_ASYNC, 1 << 26, kb.c_str()),
         other_replica =
             open_store((root / "other-replica").string(), DF_ASYNC, 1 << 26, kb.c_str());
    auto watch = df_watch_query(replica, {nullptr, 0, true}, nullptr, nullptr);
    require(watch, "watch");
    auto result = watch->result;
    unsigned notifications = 0;
    auto x = df_network_open(argv[1], uint16_t(std::stoi(argv[2])), &error),
         y = df_network_open(argv[1], uint16_t(std::stoi(argv[2])), &error);
    require(x && y, "networks");
    require(!df_network_measure(x, 1000, &error), "measure");
    require(df_network_push(x, owner, a, &error) &&
                df_network_subscribe(x, other_replica, kb.c_str(), nullptr, nullptr, &error) &&
                df_network_push(y, other, b, &error) &&
                df_network_subscribe(
                    y, replica, ka.c_str(),
                    [](void *p, const uint64_t *, size_t n) {
                      if (n)
                        ++*static_cast<unsigned *>(p);
                    },
                    &notifications, &error),
            "channels");
    auto wait = [&](auto condition) {
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(45);
      while (!condition() || !df_network_settled(x) || !df_network_settled(y)) {
        df_network_pump(x, 2000);
        df_network_pump(y, 2000);
        if (std::chrono::steady_clock::now() > deadline)
          throw std::runtime_error("Timeout: " + std::string(df_network_error(x)) + " / " +
                                   std::string(df_network_error(y)));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    };
    wait([&] { return true; });
    // First establish a signed baseline, then exercise only the fast path.
    edit_store(owner, {{1, DF_ATTR_TITLE, "baseline"}}, {});
    wait([&] { return df_view_size(result) == 1; });
    auto responses = df_network_statistics(x)->request_count;
    auto requests = df_network_statistics(x)->requests,
         uploads = df_network_statistics(x)->optimistic_uploads,
         publications = df_network_statistics(y)->optimistic_publications;
    edit_store(owner, {{1, DF_ATTR_TITLE, "one trip"}}, {});
    wait([&] { return export_records(owner) == export_records(replica); });
    if (df_network_statistics(x)->requests != requests + 1 ||
        df_network_statistics(x)->optimistic_uploads != uploads + 1 ||
        df_network_statistics(y)->optimistic_publications != publications + 1)
      throw std::runtime_error(
          "Steady update did not use one upload and direct publication: requests=" +
          std::to_string(requests) + "->" + std::to_string(df_network_statistics(x)->requests) +
          ", uploads=" + std::to_string(uploads) + "->" +
          std::to_string(df_network_statistics(x)->optimistic_uploads) +
          ", publications=" + std::to_string(publications) + "->" +
          std::to_string(df_network_statistics(y)->optimistic_publications));
    if (df_network_statistics(x)->request_count != responses)
      throw std::runtime_error("Upload produced a success reply");
    size_t sample_count = 0;
    auto samples = df_network_samples(x, &sample_count);
    bool measured = false;
    for (size_t i = 0; i < sample_count; ++i)
      if (auto &t = samples[i]; true)
        if (t.operation == 6 && t.snapshot_revision == df_revision(owner))
          measured = t.sent_us > 0 && t.elapsed_us == 0;
    if (!measured)
      throw std::runtime_error("Missing send-only timing");
    // Another connection changes the server behind this owner's journal. Queue
    // subsequent edits before recovery; reconcile the newest owner state in full.
    {
      auto rogue = open_store((root / "rogue").string(), DF_ASYNC, 1 << 26, ka.c_str());
      edit_store(rogue, {{7000, DF_ATTR_TITLE, "server-only"}}, {});
      auto z = df_network_open(argv[1], uint16_t(std::stoi(argv[2])), &error);
      require(z && df_network_push(z, rogue, a, &error), "rogue network");
      auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
      while (export_records(rogue) != export_records(replica) || !df_network_settled(z)) {
        df_network_pump(z, 2000);
        df_network_pump(y, 2000);
        if (std::chrono::steady_clock::now() > deadline)
          throw std::runtime_error("Rogue sync timeout");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      df_network_close(z);
      close_store(rogue);
    }
    auto owner_fallback = df_network_statistics(x)->optimistic_fallbacks;
    for (int i = 0; i < 20; i++) {
      edit_store(owner, {{1, DF_ATTR_TITLE, "pipeline " + std::to_string(i)}}, {});
      df_network_pump(x, 2000);
    }
    wait([&] { return export_records(owner) == export_records(replica); });
    if (df_network_statistics(x)->optimistic_fallbacks <= owner_fallback)
      throw std::runtime_error("Missing owner recovery");
    // A corrupt replica must reject the candidate transaction and reconcile.
    edit_store(replica, {{9000, DF_ATTR_TITLE, "replica-only"}}, {});
    auto fallback = df_network_statistics(y)->optimistic_fallbacks;
    edit_store(owner, {{1, DF_ATTR_TITLE, "repair subscriber"}}, {});
    wait([&] { return export_records(owner) == export_records(replica); });
    if (df_network_statistics(y)->optimistic_fallbacks != fallback + 1)
      throw std::runtime_error("Missing subscriber fallback");
    std::vector<fact> changes;
    for (int i = 1; i <= 300; i++)
      changes.push_back({uint32_t(i), DF_ATTR_TITLE, std::string(1000, 'x')});
    edit_store(owner, std::move(changes));
    edit_store(other, {{1, DF_ATTR_TITLE, "peer"}}, {});
    wait([&] {
      return df_view_size(result) == 300 && export_records(other) == export_records(other_replica);
    });
    if (export_records(owner) != export_records(replica))
      throw std::runtime_error("Initial mismatch");
    df_network_online(y, 0);
    edit_store(owner, {{1, DF_ATTR_TITLE, "edited 🌍"}},
               {{2, DF_ATTR_TITLE, std::string(1000, 'x')}});
    for (int i = 0; i < 100; i++) {
      df_network_pump(x, 2000);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    df_network_online(y, 1);
    wait([&] {
      return df_view_size(result) == 299 && export_records(owner) == export_records(replica);
    });
    // Several revisions while snapshots/fetches are active: stale pages never publish
    // into a newer exchange; eventual contents equal the current owner exactly.
    for (int i = 0; i < 50; i++) {
      edit_store(owner, {{1, DF_ATTR_TITLE, std::to_string(i)}}, {});
      df_network_pump(x, 2000);
      df_network_pump(y, 2000);
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    wait([&] { return export_records(owner) == export_records(replica); });
    auto facts = export_records(owner);
    std::map<uint64_t, std::optional<std::string>> erase;
    for (auto &[k, v] : facts)
      erase[k] = {};
    import_records(owner, erase);
    wait([&] { return !df_view_size(result); });
    if (!notifications || df_network_statistics(x)->signed_batches == 0 ||
        df_network_statistics(y)->signed_batches == 0)
      throw std::runtime_error("Missing publications/signatures");
    std::cout << "PASS native TCP: signed empty creation, one-trip optimistic updates, direct "
                 "publication, mismatch recovery, multiplexing, paged sync, Unicode, deletion, "
                 "offline/reconnect, edits during sync, clear; sent="
              << df_network_statistics(x)->sent + df_network_statistics(y)->sent << " received="
              << df_network_statistics(x)->received + df_network_statistics(y)->received
              << " notifications=" << notifications << "\n";
    df_network_close(x);
    df_network_close(y);
    close_store(owner);
    close_store(replica);
    close_store(other);
    close_store(other_replica);
    df_identity_close(a);
    df_identity_close(b);
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
