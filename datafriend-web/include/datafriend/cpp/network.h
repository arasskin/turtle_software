#pragma once
#include "../engine.h"
#include "../identity.h"
#include <chrono>
#include <exception>
namespace datafriend {
inline std::string public_key_hex(const identity &key) {
  constexpr char hex[] = "0123456789abcdef";
  std::string out;
  for (auto b : key.public_key()) {
    out += hex[b >> 4];
    out += hex[b & 15];
  }
  return out;
}
class network {
  struct callback {
    std::function<void(const std::vector<uint64_t> &)> fn;
    network *owner;
  };
  std::exception_ptr callback_error_;
  void *handle_;
  std::vector<std::unique_ptr<callback>> callbacks_;
  static void publish(void *p, const uint64_t *ids, size_t size) noexcept {
    // Never unwind through the C driver. Surface callback exceptions on pump().
    auto *c = static_cast<callback *>(p);
    try {
      if (c->fn)
        c->fn(std::vector<uint64_t>(ids, ids + size));
    } catch (...) {
      if (!c->owner->callback_error_)
        c->owner->callback_error_ = std::current_exception();
    }
  }

public:
  using request_timing = df_request_timing;
  struct statistics : df_network_stats {
    std::vector<request_timing> request_samples;
  };

private:
  mutable statistics stats_{};
  mutable std::string error_;

public:
  network(std::string address, uint16_t port) {
    detail::buffer b;
    handle_ = df_network_open(address.c_str(), port, &b.value);
    if (!handle_)
      throw std::runtime_error(b.take());
  }
  ~network() { df_network_close(handle_); }
  network(const network &) = delete;
  network &operator=(const network &) = delete;
  uint32_t push(engine &db, identity &id) {
    detail::buffer b;
    auto c = df_network_push(handle_, db.native_handle(), id.native_handle(), &b.value);
    if (!c)
      throw std::runtime_error(b.take());
    return c;
  }
  uint32_t subscribe(engine &db, std::string key,
                     std::function<void(const std::vector<uint64_t> &)> f = {}) {
    auto cb = std::make_unique<callback>();
    cb->fn = std::move(f);
    cb->owner = this;
    callbacks_.reserve(callbacks_.size() + 1);
    detail::buffer b;
    auto id =
        df_network_subscribe(handle_, db.native_handle(), key.c_str(), publish, cb.get(), &b.value);
    if (!id)
      throw std::runtime_error(b.take());
    callbacks_.push_back(std::move(cb));
    return id;
  }
  void pump(std::chrono::microseconds budget = std::chrono::microseconds(1500)) {
    if (budget.count() < 0)
      throw std::invalid_argument("negative pump budget");
    df_network_pump(handle_, uint64_t(budget.count()));
    if (callback_error_)
      std::rethrow_exception(std::exchange(callback_error_, {}));
  }
  void measure_requests(std::chrono::milliseconds interval = {}) {
    if (interval.count() < 0)
      throw std::invalid_argument("negative heartbeat interval");
    detail::buffer b;
    detail::check(df_network_measure(handle_, uint64_t(interval.count()), &b.value), b);
  }
  void online(bool enabled) { df_network_online(handle_, enabled); }
  bool connected() const { return df_network_connected(handle_); }
  bool settled() const { return df_network_settled(handle_); }
  const std::string &error() const {
    error_ = df_network_error(handle_);
    return error_;
  }
  const statistics &stats() const {
    static_cast<df_network_stats &>(stats_) = *df_network_statistics(handle_);
    size_t n;
    auto *samples = df_network_samples(handle_, &n);
    if (n)
      stats_.request_samples.assign(samples, samples + n);
    else
      stats_.request_samples.clear();
    return stats_;
  }
};
} // namespace datafriend
