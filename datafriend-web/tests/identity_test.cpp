#include "datafriend/c_api.h"
#include "datafriend/wire_codec.h"
#include "datafriend/attributes.h"
#include "monocypher-ed25519.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace datafriend;
void check(bool ok) { if(!ok) throw std::runtime_error("identity test failed"); }
std::string unhex(std::string_view text) {
  std::string bytes;
  for(size_t i = 0; i < text.size(); i += 2)
    bytes.push_back(char(std::stoi(std::string(text.substr(i, 2)), nullptr, 16)));
  return bytes;
}
struct buffer {
  df_buffer out{};
  ~buffer() { df_free(out.data); }
  std::string str() const { return {reinterpret_cast<char*>(out.data), out.size}; }
};
std::string envelope(std::string_view key) {
  writer w; w.data = std::string("LBS\5", 4);
  w.u32(1); w.u32(2); w.u8(3);
  w.blob(key); w.blob(attribute_schema); w.u64(123); w.u64(456);
  w.blob(std::string(256 * 1024, 'x')); w.data.append(32, 'c'); w.u64(0);
  return w.data;
}
int main() {
  // RFC 8032 test vector 1, independently published seed/public/signature.
  auto seed = unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
  auto expected = unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
  auto signed_empty = unhex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555f"
                           "b8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
  uint8_t secret[64], pub[32], signature[64];
  crypto_ed25519_key_pair(secret, pub, reinterpret_cast<uint8_t*>(seed.data()));
  crypto_ed25519_sign(signature, secret, nullptr, 0);
  check(std::memcmp(pub, expected.data(), 32) == 0);
  check(std::memcmp(signature, signed_empty.data(), 64) == 0);
  check(seed == std::string(32, '\0'));
  crypto_wipe(secret, sizeof secret);

  buffer a, b;
  void* first = df_identity_open(nullptr, 0, 1, &a.out);
  void* second = df_identity_open(nullptr, 0, 1, &b.out);
  check(first && second && a.out.size == 32 && b.out.size == 32 && a.str() != b.str());
  auto packet = envelope(a.str());
  buffer proof;
  check(df_identity_sign(first, reinterpret_cast<const uint8_t*>(packet.data()), packet.size(), &proof.out) == 0);
  check(proof.out.size == 64);
  check(crypto_ed25519_check(proof.out.data, a.out.data,
    reinterpret_cast<const uint8_t*>(packet.data()), packet.size()) == 0);
  auto optimistic=packet;optimistic[12]=6;buffer fast_proof;
  check(df_identity_sign(first,reinterpret_cast<const uint8_t*>(optimistic.data()),optimistic.size(),&fast_proof.out)==0);
  check(crypto_ed25519_check(fast_proof.out.data,a.out.data,reinterpret_cast<const uint8_t*>(optimistic.data()),optimistic.size())==0);
  check(crypto_ed25519_check(fast_proof.out.data,a.out.data,reinterpret_cast<const uint8_t*>(packet.data()),packet.size())!=0);
  packet.back() ^= 1;
  check(crypto_ed25519_check(proof.out.data, a.out.data,
    reinterpret_cast<const uint8_t*>(packet.data()), packet.size()) != 0);
  for(auto bad : {envelope(b.str()), std::string("arbitrary protocol"),
                 packet.substr(0, packet.size() - 1), packet + "trailing"}) {
    buffer error;
    check(df_identity_sign(first, reinterpret_cast<const uint8_t*>(bad.data()), bad.size(), &error.out) != 0);
  }
  for(size_t offset : {size_t(3), size_t(7), size_t(11), size_t(12)}) {
    auto bad = packet;
    bad[offset] = 0;
    buffer error;
    check(df_identity_sign(first, reinterpret_cast<const uint8_t*>(bad.data()), bad.size(), &error.out) != 0);
  }
  buffer invalid;
  check(!df_identity_open(reinterpret_cast<const uint8_t*>("../bad"), 6, 0, &invalid.out));
  df_identity_close(first); df_identity_close(second); df_identity_close(nullptr);
  std::cout << "native identities: RFC8032, isolated handles, signature verification, envelope rejection passed\n";
}
