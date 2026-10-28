#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <stdexcept>
#include <limits>
namespace wire {
struct writer {
  std::string data;
  void u8(uint8_t n) { data.push_back(char(n)); }
  void u32(uint32_t n) {
    for (int i = 3; i >= 0; --i)
      u8(n >> (i * 8));
  }
  void u64(uint64_t n) {
    for (int i = 7; i >= 0; --i)
      u8(n >> (i * 8));
  }
  void blob(std::string_view b) {
    if (b.size() > UINT32_MAX)
      throw std::length_error("field too large");
    u32(uint32_t(b.size()));
    data.append(b);
  }
};
struct reader {
  std::string_view data;
  uint8_t u8() {
    if (data.empty())
      throw std::invalid_argument("truncated message");
    auto b = uint8_t(data.front());
    data.remove_prefix(1);
    return b;
  }
  uint32_t u32() {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
      v = (v << 8) | u8();
    return v;
  }
  uint64_t u64() {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
      v = (v << 8) | u8();
    return v;
  }
  std::string_view take(size_t n) {
    if (n > data.size())
      throw std::invalid_argument("truncated field");
    auto b = data.substr(0, n);
    data.remove_prefix(n);
    return b;
  }
  std::string blob() { return std::string(take(u32())); }
  uint32_t count(size_t min_bytes = 1) {
    auto n = u32();
    if (n > data.size() / min_bytes)
      throw std::invalid_argument("invalid item count");
    return n;
  }
  void end() {
    if (!data.empty())
      throw std::invalid_argument("trailing message data");
  }
};
} // namespace wire
