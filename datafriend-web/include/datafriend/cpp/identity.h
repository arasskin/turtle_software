#pragma once
#include <array>
#include <string_view>
#include <string>
#include <stdexcept>
#include <cstring>
namespace datafriend {
class identity {
  void *handle_ = nullptr;
  std::array<uint8_t, 32> public_{};
  static void fail(df_buffer out) {
    std::string s(reinterpret_cast<char *>(out.data), out.size);
    df_free(out.data);
    throw std::runtime_error(s);
  }

public:
  identity(std::string_view alias, bool ephemeral) {
    df_buffer out{};
    handle_ = df_identity_open(reinterpret_cast<const uint8_t *>(alias.data()), alias.size(),
                               ephemeral, &out);
    if (!handle_)
      fail(out);
    std::memcpy(public_.data(), out.data, 32);
    df_free(out.data);
  }
  ~identity() { df_identity_close(handle_); }
  identity(const identity &) = delete;
  identity &operator=(const identity &) = delete;
  const auto &public_key() const { return public_; }
  void *native_handle() const { return handle_; }
  std::array<uint8_t, 64> sign_write(std::string_view packet) const {
    df_buffer out{};
    if (df_identity_sign(handle_, reinterpret_cast<const uint8_t *>(packet.data()), packet.size(),
                         &out))
      fail(out);
    std::array<uint8_t, 64> signature;
    std::memcpy(signature.data(), out.data, 64);
    df_free(out.data);
    return signature;
  }
};
} // namespace datafriend
