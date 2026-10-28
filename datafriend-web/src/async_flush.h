#pragma once
// Compatibility adapter for existing C++ callers/tests; worker is C99.
#include "flush.h"
#include <chrono>
#include <functional>
#include <system_error>
#include <thread>
namespace datafriend {
class async_flush {
  std::function<int()> sync_;
  mutable df_flush state_{};
  static int sync(void *p) noexcept {
    try {
      return static_cast<async_flush *>(p)->sync_();
    } catch (...) {
      return EIO;
    }
  }
  static void check(int rc) {
    if (rc)
      throw std::system_error(rc, std::generic_category(), "Datafriend disk flush failed");
  }

public:
  explicit async_flush(std::function<int()> f,
                       std::chrono::milliseconds interval = std::chrono::milliseconds(100))
      : sync_(std::move(f)) {
    if (interval.count() < 0)
      throw std::invalid_argument("negative flush interval");
    check(df_flush_init(&state_, sync, this, uint64_t(interval.count())));
  }
  ~async_flush() { df_flush_destroy(&state_); }
  async_flush(const async_flush &) = delete;
  async_flush &operator=(const async_flush &) = delete;
  void written() { df_flush_written(&state_); }
  void check() const { check(df_flush_check(&state_)); }
  void flush() { check(df_flush_barrier(&state_)); }
  int stop() noexcept { return df_flush_stop(&state_); }
  uint64_t attempts() const { return df_flush_attempts(&state_); }
};
} // namespace datafriend
