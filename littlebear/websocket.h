#ifndef LB_WEBSOCKET_H
#define LB_WEBSOCKET_H
/* RFC 6455 transport only. Binary payloads form the existing LBS byte stream.
 * No extensions, compression, TLS, HTTP routing, or application authentication. */
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum { LB_WS_BUFFER = 8192 };
typedef struct lb_websocket {
  /* Also used for the bounded HTTP upgrade; allocated only for WS clients. */
  uint8_t buffer[LB_WS_BUFFER];
  size_t at, count;
  bool upgraded, raw_output, close_after_send, fragmented;
  uint8_t header[14], control[125], opcode;
  size_t have, need, control_size;
  uint64_t remaining, offset;
} lb_websocket;

static inline uint32_t lb_ws_rotate(uint32_t x, unsigned n) {
  return (x << n) | (x >> (32 - n));
}
/* SHA-1 is required solely for the public HTTP upgrade accept value. This
 * fixed-size implementation hashes the 24-byte key plus RFC GUID (60 bytes).
 * Littlebear authentication still uses its existing Ed25519 implementation. */
static inline void lb_ws_accept(const char key[24], char out[29]) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  uint8_t bytes[128] = {0}, digest[20];
  uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
  memcpy(bytes, key, 24);
  memcpy(bytes + 24, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);
  bytes[60] = 0x80; bytes[126] = 1; bytes[127] = 0xe0;
  for (size_t block = 0; block < 128; block += 64) {
    uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (unsigned i = 0; i < 16; ++i)
      w[i] = (uint32_t)bytes[block + i*4] << 24 | (uint32_t)bytes[block + i*4+1] << 16 |
             (uint32_t)bytes[block + i*4+2] << 8 | bytes[block + i*4+3];
    for (unsigned i = 16; i < 80; ++i)
      w[i] = lb_ws_rotate(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    for (unsigned i = 0; i < 80; ++i) {
      uint32_t f, k, t;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
      else { f = b ^ c ^ d; k = 0xca62c1d6; }
      t = lb_ws_rotate(a, 5) + f + e + k + w[i];
      e = d; d = c; c = lb_ws_rotate(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  for (unsigned i = 0; i < 20; ++i) digest[i] = (uint8_t)(h[i/4] >> (24 - 8*(i%4)));
  for (unsigned i = 0, j = 0; i < 20; i += 3) {
    uint32_t v = (uint32_t)digest[i] << 16;
    if (i+1 < 20) v |= (uint32_t)digest[i+1] << 8;
    if (i+2 < 20) v |= digest[i+2];
    out[j++] = alphabet[v >> 18]; out[j++] = alphabet[(v >> 12) & 63];
    out[j++] = alphabet[(v >> 6) & 63]; out[j++] = i+2 < 20 ? alphabet[v & 63] : '=';
  }
  out[28] = 0;
}
static inline bool lb_ws_equal(const char *a, const char *b) {
  while (*a && *b) {
    unsigned x = (unsigned char)*a++, y = (unsigned char)*b++;
    if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
    if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
    if (x != y) return false;
  }
  return *a == *b;
}
static inline bool lb_ws_token(char *value, const char *wanted, bool fold) {
  for (char *p = value; *p;) {
    char *end = strchr(p, ','), *tail;
    char saved;
    if (!end) end = p + strlen(p);
    while (*p == ' ' || *p == '\t') ++p;
    tail = end;
    while (tail > p && (tail[-1] == ' ' || tail[-1] == '\t')) --tail;
    saved = *tail; *tail = 0;
    bool match = fold ? lb_ws_equal(p, wanted) : !strcmp(p, wanted);
    *tail = saved;
    if (match) return true;
    p = *end ? end + 1 : end;
  }
  return false;
}
/* Mutates a complete CRLF-terminated HTTP header. Never reads past length.
 * Return HTTP status. Only the optional "binary" subprotocol is selected. */
static inline int lb_ws_upgrade(uint8_t *bytes, size_t length, char response[256]) {
  char *p = (char *)bytes, *line, *key = NULL, *version = NULL;
  bool host = false, upgrade = false, connection = false, binary = false;
  char accept[29];
  if (length < 4 || memcmp(bytes + length - 4, "\r\n\r\n", 4)) return 400;
  for (size_t i = 0; i < length; ++i)
    if (!bytes[i] || bytes[i] == 127 || (bytes[i] < 32 && bytes[i] != '\r' && bytes[i] != '\n' && bytes[i] != '\t')) return 400;
  bytes[length-2] = 0;
  line = strstr(p, "\r\n");
  if (!line) return 400;
  *line = 0;
  if (strncmp(p, "GET /", 5)) return 400;
  char *space = strchr(p + 4, ' ');
  if (!space || strcmp(space, " HTTP/1.1")) return 400;
  p = line + 2;
  while (*p) {
    char *colon, *value, *tail;
    line = strstr(p, "\r\n");
    if (!line) return 400;
    *line = 0;
    colon = strchr(p, ':');
    if (!colon || colon == p) return 400;
    for (char *q = p; q < colon; ++q)
      if (!((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
            (*q >= '0' && *q <= '9') || strchr("!#$%&'*+-.^_`|~", *q))) return 400;
    *colon = 0;
    value = colon + 1;
    while (*value == ' ' || *value == '\t') ++value;
    tail = value + strlen(value);
    while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
    if (strchr(value, '\r') || strchr(value, '\n')) return 400;
    if (lb_ws_equal(p, "Host")) { if (host || !*value) return 400; host = true; }
    else if (lb_ws_equal(p, "Upgrade")) upgrade |= lb_ws_token(value, "websocket", true);
    else if (lb_ws_equal(p, "Connection")) connection |= lb_ws_token(value, "upgrade", true);
    else if (lb_ws_equal(p, "Sec-WebSocket-Key")) { if (key) return 400; key = value; }
    else if (lb_ws_equal(p, "Sec-WebSocket-Version")) { if (version) return 400; version = value; }
    else if (lb_ws_equal(p, "Sec-WebSocket-Protocol")) binary |= lb_ws_token(value, "binary", false);
    else if (lb_ws_equal(p, "Transfer-Encoding") || (lb_ws_equal(p, "Content-Length") && strcmp(value, "0"))) return 400;
    p = line + 2;
  }
  if (!host || !upgrade || !connection || !key || !version) return 400;
  if (strcmp(version, "13")) return 426;
  if (strlen(key) != 24 || key[22] != '=' || key[23] != '=') return 400;
  for (unsigned i = 0; i < 22; ++i)
    if (!strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/", key[i])) return 400;
  /* A canonical base64 encoding of exactly 16 bytes has four zero pad bits. */
  if (!strchr("AQgw", key[21])) return 400;
  lb_ws_accept(key, accept);
  snprintf(response, 256, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n%s\r\n",
           accept, binary ? "Sec-WebSocket-Protocol: binary\r\n" : "");
  return 101;
}
static inline size_t lb_ws_header(uint8_t out[10], uint8_t opcode, uint64_t length) {
  size_t n = length < 126 ? 2 : length <= 65535 ? 4 : 10;
  out[0] = 0x80 | opcode;
  out[1] = n == 2 ? (uint8_t)length : n == 4 ? 126 : 127;
  for (size_t i = 2; i < n; ++i) out[i] = (uint8_t)(length >> (8*(n-i-1)));
  return n;
}
static inline bool lb_ws_utf8(const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n;) {
    uint32_t code = p[i++], minimum;
    unsigned more;
    if (code < 128) continue;
    if (code >= 0xc2 && code <= 0xdf) { more = 1; minimum = 0x80; code &= 31; }
    else if (code >= 0xe0 && code <= 0xef) { more = 2; minimum = 0x800; code &= 15; }
    else if (code >= 0xf0 && code <= 0xf4) { more = 3; minimum = 0x10000; code &= 7; }
    else return false;
    if (more > n-i) return false;
    while (more--) { if ((p[i] & 0xc0) != 0x80) return false; code = (code << 6) | (p[i++] & 63); }
    if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
  }
  return true;
}
static inline int lb_ws_end(lb_websocket *w) {
  w->have = 0;
  if (w->opcode < 8) return 0;
  if (w->opcode == 8) {
    unsigned code;
    if (w->control_size == 1) return -1002;
    if (w->control_size >= 2) {
      code = (unsigned)w->control[0] << 8 | w->control[1];
      if (!((code >= 1000 && code <= 1014 && code != 1004 && code != 1005 && code != 1006) ||
            (code >= 3000 && code <= 4999))) return -1002;
      if (!lb_ws_utf8(w->control + 2, w->control_size - 2)) return -1007;
    }
  }
  return w->opcode;
}
/* Bulk application payload path. Caller supplies room in the native packet;
 * framing/control bytes still go through lb_ws_byte. No extra payload buffer. */
static inline size_t lb_ws_data(lb_websocket *w, const uint8_t *source, uint8_t *destination, size_t size) {
  if (size > w->remaining) size = (size_t)w->remaining;
  const uint8_t *mask = w->header + w->need - 4;
  for (size_t i = 0; i < size; ++i) destination[i] = source[i] ^ mask[(w->offset + i) & 3];
  w->offset += size;
  w->remaining -= size;
  if (!w->remaining) lb_ws_end(w);
  return size;
}
/* Consume one wire byte. 1 emits one unmasked application byte; 8/9/10 emit
 * complete controls; negative values request a close with that status code.
 * Payloads stream through: fragmented messages never require accumulation. */
static inline int lb_ws_byte(lb_websocket *w, uint8_t byte, uint8_t *data, uint64_t limit) {
  if (!w->remaining) {
    if (!w->have) w->need = 2;
    w->header[w->have++] = byte;
    if (w->have == 2) {
      unsigned op = w->header[0] & 15, n = byte & 127;
      bool fin = (w->header[0] & 128) != 0;
      if ((w->header[0] & 0x70) || !(byte & 128) ||
          (op != 0 && op != 1 && op != 2 && op != 8 && op != 9 && op != 10) ||
          (op >= 8 && (!fin || n > 125)) ||
          (op == 0 && !w->fragmented) || (op < 8 && op && w->fragmented)) return -1002;
      if (op == 1) return -1003; /* LBS is binary. */
      w->need = 6 + (n == 126 ? 2 : n == 127 ? 8 : 0);
    }
    if (w->have != w->need) return 0;
    unsigned n = w->header[1] & 127;
    uint64_t length = n;
    if (n >= 126) {
      length = 0;
      for (size_t i = 2; i < w->need - 4; ++i) length = (length << 8) | w->header[i];
      if ((n == 126 && length < 126) || (n == 127 && (length < 65536 || (w->header[2] & 128)))) return -1002;
    }
    if (length > limit) return -1009;
    w->opcode = w->header[0] & 15;
    if (w->opcode < 8) w->fragmented = !(w->header[0] & 128);
    w->remaining = length; w->offset = 0; w->control_size = 0;
    return length ? 0 : lb_ws_end(w);
  }
  byte ^= w->header[w->need - 4 + (w->offset++ & 3)];
  --w->remaining;
  if (w->opcode >= 8) {
    w->control[w->control_size++] = byte;
    return w->remaining ? 0 : lb_ws_end(w);
  }
  *data = byte;
  if (!w->remaining) lb_ws_end(w);
  return 1;
}
#endif
