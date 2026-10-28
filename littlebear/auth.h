#ifndef LB_AUTH_H
#define LB_AUTH_H
#include "protocol.h"
#include <sodium.h>
typedef struct lb_auth {
  uint8_t challenge[32];
  uint64_t sequence;
  bool issued, pushing;
} lb_auth;
static inline bool lb_auth_namespace(lb_bytes key, uint8_t name[16]) {
  return crypto_generichash(name, 16, key.data, key.size, NULL, 0) == 0;
}
static inline bool lb_auth_challenge(lb_auth *a, lb_buffer *reply) {
  if (reply->size < 17 || !lb_buffer_reserve(reply, reply->size + 32))
    return false;
  randombytes_buf(a->challenge, sizeof(a->challenge));
  a->sequence = 0;
  a->issued = true;
  memmove(reply->data + 49, reply->data + 17, reply->size - 17);
  memcpy(reply->data + 17, a->challenge, 32);
  reply->size += 32;
  reply->data[0] |= 0x80;
  return true;
}
static inline bool lb_auth_verify(lb_auth *a, const lb_request *r) {
  if ((r->op != 3 && r->op != 6) || !a->issued || r->sequence != a->sequence ||
      r->challenge.size != 32 || r->signature.size != crypto_sign_BYTES ||
      r->public_key.size != crypto_sign_PUBLICKEYBYTES ||
      sodium_memcmp(r->challenge.data, a->challenge, 32) ||
      crypto_sign_verify_detached(r->signature.data, r->signed_bytes.data, r->signed_bytes.size,
                                  r->public_key.data))
    return false;
  if (a->sequence == UINT64_MAX)
    a->issued = false;
  else
    ++a->sequence;
  return true;
}
#endif
