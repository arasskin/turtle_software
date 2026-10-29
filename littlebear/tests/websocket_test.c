#include "../websocket.h"
#include <assert.h>
#include <stdlib.h>

static const char request[] =
    "GET /sync HTTP/1.1\r\nHost: localhost\r\nUpgrade: WebSocket\r\n"
    "Connection: keep-alive, Upgrade\r\nSec-WebSocket-Version: 13\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Protocol: other, binary\r\n\r\n";
static int upgrade(const char *text, char *response) {
  uint8_t copy[8192];
  memcpy(copy, text, strlen(text));
  return lb_ws_upgrade(copy, strlen(text), response);
}
static int frame(lb_websocket *w, unsigned opcode, bool fin, const uint8_t *data,
                 size_t size, uint8_t *output, size_t *count) {
  uint8_t header[14], byte = 0;
  size_t n = lb_ws_header(header, (uint8_t)opcode, size);
  int event = 0;
  if (!fin) header[0] &= 127;
  header[1] |= 128;
  memcpy(header + n, "mask", 4);
  for (size_t i = 0; i < n + 4; ++i) {
    event = lb_ws_byte(w, header[i], &byte, 8*1024*1024+4);
    if (event < 0) return event;
  }
  for (size_t i = 0; i < size; ++i) {
    event = lb_ws_byte(w, data[i] ^ (uint8_t)"mask"[i%4], &byte, 8*1024*1024+4);
    if (event == 1) output[(*count)++] = byte;
    if (event < 0) return event;
  }
  return event;
}
int main(void) {
  char response[256], accept[29], altered[8192];
  uint8_t output[70000], source[70000], byte;
  size_t count;
  lb_websocket w;
  lb_ws_accept("dGhlIHNhbXBsZSBub25jZQ==", accept);
  assert(!strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
  assert(upgrade(request, response) == 101);
  assert(strstr(response, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"));
  assert(strstr(response, "Sec-WebSocket-Protocol: binary\r\n"));
  strcpy(altered, request); *strstr(altered, "13") = '2';
  assert(upgrade(altered, response) == 426);
  strcpy(altered, request); *strstr(altered, "GET") = 'P';
  assert(upgrade(altered, response) == 400);
  strcpy(altered, request); strstr(altered, "ZQ==")[1] = 'R';
  assert(upgrade(altered, response) == 400);
  strcpy(altered, request); strcpy(altered + strlen(altered)-2, "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
  assert(upgrade(altered, response) == 400);
  strcpy(altered, request); *strstr(altered, "Host:") = ' ';
  assert(upgrade(altered, response) == 400);
  for (size_t i = 0; i < sizeof(source); ++i) source[i] = (uint8_t)(i*71);
  const size_t sizes[] = {0, 1, 125, 126, 65535, 65536, 70000};
  for (size_t i = 0; i < sizeof(sizes)/sizeof(*sizes); ++i) {
    memset(&w, 0, sizeof(w)); count = 0;
    assert(frame(&w, 2, true, source, sizes[i], output, &count) >= 0);
    assert(count == sizes[i] && !memcmp(source, output, count) && !w.have && !w.remaining);
  }
  memset(&w, 0, sizeof(w)); count = 0;
  assert(frame(&w, 2, false, source, 71, output, &count) == 1);
  assert(frame(&w, 9, true, source, 3, output, &count) == 9);
  assert(w.control_size == 3 && !memcmp(w.control, source, 3));
  assert(frame(&w, 0, true, source+71, 1000, output, &count) == 1);
  assert(count == 1071 && !memcmp(output, source, count) && !w.fragmented);
  assert(frame(&w, 0, true, source, 1, output, &count) == -1002);
  memset(&w, 0, sizeof(w));
  assert(frame(&w, 1, true, source, 1, output, &count) == -1003);
  memset(&w, 0, sizeof(w));
  assert(frame(&w, 9, false, source, 1, output, &count) == -1002);
  memset(&w, 0, sizeof(w));
  assert(frame(&w, 9, true, source, 126, output, &count) == -1002);
  memset(&w, 0, sizeof(w));
  assert(frame(&w, 8, true, (const uint8_t *)"\3\350bye", 5, output, &count) == 8);
  memset(&w, 0, sizeof(w));
  assert(frame(&w, 8, true, (const uint8_t *)"x", 1, output, &count) == -1002);
  memset(&w, 0, sizeof(w));
  assert(frame(&w, 8, true, (const uint8_t *)"\3\355", 2, output, &count) == -1002);
  memset(&w, 0, sizeof(w));
  assert(frame(&w, 8, true, (const uint8_t *)"\3\350\300\200", 4, output, &count) == -1007);
  memset(&w, 0, sizeof(w));
  assert(lb_ws_byte(&w, 0x82, &byte, 100) == 0);
  assert(lb_ws_byte(&w, 0, &byte, 100) == -1002); /* unmasked */
  memset(&w, 0, sizeof(w));
  const uint8_t long_length[] = {0x82, 0xff, 0x80, 0, 0, 0, 0, 1, 0, 0, 1, 2, 3, 4};
  for (size_t i = 0; i < sizeof(long_length); ++i)
    assert(lb_ws_byte(&w, long_length[i], &byte, UINT64_MAX) == (i+1 == sizeof(long_length) ? -1002 : 0));
  /* The bulk unmask path crosses mask boundaries in arbitrary chunk sizes. */
  memset(&w, 0, sizeof(w));
  uint8_t header[14], masked[70000];
  size_t n = lb_ws_header(header, 2, sizeof(source));
  header[1] |= 128; memcpy(header+n, "mask", 4);
  for (size_t i = 0; i < n+4; ++i) assert(!lb_ws_byte(&w, header[i], &byte, sizeof(source)));
  for (size_t i = 0; i < sizeof(source); ++i) masked[i] = source[i] ^ (uint8_t)"mask"[i%4];
  count = 0;
  while (count < sizeof(source)) {
    size_t amount = sizeof(source)-count;
    if (amount > 71) amount = 71;
    count += lb_ws_data(&w, masked+count, output+count, amount);
  }
  assert(!w.have && !w.remaining && !memcmp(source, output, sizeof(source)));
  puts("websocket: handshake, length boundaries, masking, fragments, controls and close validation passed");
  return 0;
}
