#include "flush.h"
#include <atomic>
#include <cerrno>
#include <future>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;
void require(bool value, const char *message) {
  if (!value)
    throw std::runtime_error(message);
}
int main() {
  try {
    {
      std::atomic<int> calls = 0;
      df_flush f{};
      require(!df_flush_init(
                  &f,
                  [](void *p) {
                    ++*static_cast<std::atomic<int> *>(p);
                    return 0;
                  },
                  &calls, 3600000),
              "init");
      for (int i = 0; i < 100; ++i)
        df_flush_written(&f);
      require(!df_flush_barrier(&f), "barrier");
      require(calls == 1, "writes were not coalesced");
      require(!df_flush_barrier(&f), "barrier");
      require(calls == 1, "clean barrier flushed again");
      df_flush_written(&f);
      require(!df_flush_stop(&f), "stop");
      require(calls == 2, "close did not drain pending writes");
      df_flush_destroy(&f);
    }
    {
      std::atomic<int> calls = 0;
      std::promise<void> entered, release;
      auto allow = release.get_future().share();
      struct context {
        std::atomic<int> *calls;
        std::promise<void> *entered;
        std::shared_future<void> *allow;
      } ctx{&calls, &entered, &allow};
      df_flush f{};
      require(!df_flush_init(
                  &f,
                  [](void *p) {
                    auto &c = *static_cast<context *>(p);
                    if (++*c.calls == 1) {
                      c.entered->set_value();
                      c.allow->wait();
                    }
                    return 0;
                  },
                  &ctx, 3600000),
              "init");
      df_flush_written(&f);
      auto barrier =
          std::async(std::launch::async, [&] { require(!df_flush_barrier(&f), "barrier"); });
      entered.get_future().wait();
      auto write = std::async(std::launch::async, [&] { df_flush_written(&f); });
      const bool nonblocking = write.wait_for(1s) == std::future_status::ready;
      // Stop while a flush is active, with a newer write pending. It must flush
      // that newer write too before allowing the fd to be closed.
      auto close = std::async(std::launch::async, [&] { return df_flush_stop(&f); });
      release.set_value();
      write.get();
      barrier.get();
      require(close.get() == 0 && calls == 2, "write during sync lost at close");
      require(nonblocking, "disk flush blocked a writer on the worker mutex");
      df_flush_destroy(&f);
    }
    {
      std::atomic<bool> fail = true;
      df_flush f{};
      require(!df_flush_init(
                  &f, [](void *p) { return *static_cast<std::atomic<bool> *>(p) ? EIO : 0; }, &fail,
                  3600000),
              "init");
      df_flush_written(&f);
      require(df_flush_barrier(&f) == EIO, "flush error swallowed");
      require(df_flush_check(&f) == EIO, "background error not visible to writers");
      fail = false;
      require(!df_flush_barrier(&f), "barrier");
      require(!df_flush_check(&f), "check");
      df_flush_destroy(&f);
    }
    {
      df_flush f{};
      require(!df_flush_init(&f, [](void *) { return 0; }, nullptr, 20), "init");
      const auto until = std::chrono::steady_clock::now() + 200ms;
      while (std::chrono::steady_clock::now() < until) {
        df_flush_written(&f);
        std::this_thread::sleep_for(1ms);
      }
      require(df_flush_attempts(&f) > 0, "continuous writes postponed automatic flushes");
      df_flush_destroy(&f);
    }
    std::cout
        << "PASS: batching, periodic progress, nonblocking writes, barriers, shutdown and errors\n";
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
