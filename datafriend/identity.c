#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "c_api.h"
#include "schema.h"
#include "codec.h"
#include "monocypher-ed25519.h"
#include <errno.h>
#ifdef __APPLE__
#include <Security/Security.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif
/* No seed export. Platform code must load an existing key or durably protect
 * the candidate before returning success. Failure never rotates an identity. */
const char *df_identity_load(const char *alias, uint8_t seed[32]);
typedef struct df_identity {
  uint8_t secret[64], public_key[32];
} df_identity;
static void error(df_buffer *out, const char *s) {
  out->size = strlen(s);
  out->data = malloc(out->size);
  if (out->data)
    memcpy(out->data, s, out->size);
  else
    out->size = 0;
}
static const char *random_seed(uint8_t seed[32]) {
#ifdef __APPLE__
  return SecRandomCopyBytes(kSecRandomDefault, 32, seed) == errSecSuccess
             ? NULL
             : "identity randomness unavailable";
#else
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return "identity randomness unavailable";
  size_t n = 0;
  while (n < 32) {
    ssize_t rc = read(fd, seed + n, 32 - n);
    if (rc < 0 && errno == EINTR)
      continue;
    if (rc <= 0) {
      close(fd);
      return "identity randomness unavailable";
    }
    n += (size_t)rc;
  }
  close(fd);
  return NULL;
#endif
}
void *df_identity_open(const uint8_t *alias, size_t size, int ephemeral, df_buffer *out) {
  uint8_t seed[32] = {0};
  char name[129];
  df_identity *key = NULL;
  const char *failure = "invalid identity options";
  *out = (df_buffer){0};
  if ((!alias && size) || size > 128 || (ephemeral != 0 && ephemeral != 1) || (!ephemeral && !size))
    goto fail;
  for (size_t i = 0; i < size; ++i) {
    unsigned c = alias[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
          c == '-' || c == '_'))
      goto fail;
  }
  if (size)
    memcpy(name, alias, size);
  name[size] = 0;
  key = calloc(1, sizeof(*key));
  out->data = malloc(32);
  if (!key || !out->data) {
    failure = "out of memory";
    goto fail;
  }
  failure = random_seed(seed);
  if (failure)
    goto fail;
  if (!ephemeral) {
    failure = df_identity_load(name, seed);
    if (failure)
      goto fail;
  }
  crypto_ed25519_key_pair(key->secret, key->public_key, seed);
  crypto_wipe(seed, sizeof(seed));
  memcpy(out->data, key->public_key, 32);
  out->size = 32;
  return key;
fail:
  crypto_wipe(seed, sizeof(seed));
  if (key) {
    crypto_wipe(key, sizeof(*key));
    free(key);
  }
  free(out->data);
  *out = (df_buffer){0};
  error(out, failure);
  return NULL;
}
int df_identity_sign(void *handle, const uint8_t *packet, size_t size, df_buffer *out) {
  df_identity *key = handle;
  *out = (df_buffer){0};
  const char *failure = "invalid owner write envelope";
  if (!key || !packet || size > 8 * 1024 * 1024 - 64 || size < 13 || memcmp(packet, "LBS\5", 4))
    goto fail;
  lb_reader r = {lb_bytes_make(packet + 4, size - 4), false};
  uint32_t request = lb_read_u32(&r), channel = lb_read_u32(&r);
  uint8_t op = lb_read_u8(&r);
  if (!request || !channel || (op != 3 && op != 6))
    goto fail;
  lb_bytes pub = lb_read_blob(&r);
  if (pub.size != 32 || crypto_verify32(pub.data, key->public_key))
    goto fail;
  if (!lb_bytes_equal(lb_read_blob(&r),
                      lb_bytes_make(DATAFRIEND_SCHEMA, sizeof(DATAFRIEND_SCHEMA) - 1)))
    goto fail;
  lb_read_u64(&r);
  lb_read_u64(&r);
  lb_read_blob(&r);
  lb_take(&r, 32);
  lb_read_u64(&r);
  if (!lb_reader_end(&r))
    goto fail;
  out->data = malloc(64);
  if (!out->data) {
    failure = "out of memory";
    goto fail;
  }
  crypto_ed25519_sign(out->data, key->secret, packet, size);
  out->size = 64;
  return 0;
fail:
  error(out, failure);
  return 1;
}
void df_identity_close(void *handle) {
  if (handle) {
    crypto_wipe(handle, sizeof(df_identity));
    free(handle);
  }
}
#if !defined(__APPLE__) && !defined(__ANDROID__)
const char *df_identity_load(const char *alias, uint8_t seed[32]) {
  (void)alias;
  (void)seed;
  return "secure identity storage is implemented on Apple and Android only";
}
#endif
const uint8_t *df_identity_public(void *p) {
  return p ? ((df_identity *)p)->public_key : NULL;
}
