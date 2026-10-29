#ifndef LB_OWNER_AUTHORIZATION_H
#define LB_OWNER_AUTHORIZATION_H
#include "models.h"
#include <sodium.h>
static inline bool lb_auth_initialize(void) { return sodium_init() >= 0; }
static inline bool lb_auth_namespace(lb_bytes key, uint8_t name[LB_NAMESPACE_BYTES]) {
  return crypto_generichash(name, LB_NAMESPACE_BYTES, key.data, key.size, NULL, 0) == 0;
}
static inline void lb_auth_challenge(lb_auth *a) {
  randombytes_buf(a->challenge, sizeof(a->challenge));
  a->sequence = 0;
  a->issued = true;
}
static inline bool lb_auth_verify(lb_auth *a, const lb_request *r) {
  if (!lb_operation_signed(r->op) || !a->issued || r->sequence != a->sequence ||
      r->challenge.size != LB_CHALLENGE_BYTES || r->signature.size != crypto_sign_BYTES ||
      r->public_key.size != crypto_sign_PUBLICKEYBYTES ||
      sodium_memcmp(r->challenge.data, a->challenge, LB_CHALLENGE_BYTES) ||
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
