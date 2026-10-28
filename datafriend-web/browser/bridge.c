#include "datafriend/db.h"
#include "datafriend/network.h"
#include <emscripten/emscripten.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char last_error[512];
static char *text_result;
static void buffer_error(df_buffer *b) {
  snprintf(last_error, sizeof(last_error), "%.*s", (int)b->size, b->data ? (char *)b->data : "");
  df_free(b->data);
}
EMSCRIPTEN_KEEPALIVE const char *dfb_error(void) { return last_error; }
EMSCRIPTEN_KEEPALIVE void *dfb_identity(void) {
  df_buffer error = {0};
  void *identity = df_identity_open(NULL, 0, 1, &error);
  if (!identity) buffer_error(&error); else df_free(error.data);
  return identity;
}
EMSCRIPTEN_KEEPALIVE const char *dfb_key(void *identity) {
  static char hex[65];
  const uint8_t *key = df_identity_public(identity);
  for (int i=0; i<32; ++i) snprintf(hex+i*2, 3, "%02x", key[i]);
  return hex;
}
EMSCRIPTEN_KEEPALIVE void dfb_identity_close(void *identity) { df_identity_close(identity); }
EMSCRIPTEN_KEEPALIVE void *dfb_open(const char *directory, const char *name) {
  df_buffer error = {0};
  df_options opts = {directory, name, DF_BUFFERED, 8*1024*1024};
  void *s = df_store_open(&opts, &error);
  if (!s) buffer_error(&error);
  return s;
}
EMSCRIPTEN_KEEPALIVE int dfb_close(df_store *s) {
  df_buffer error = {0};
  int status = df_store_close(s, &error);
  if (status != DF_OK) buffer_error(&error);
  return status;
}
EMSCRIPTEN_KEEPALIVE double dfb_revision(void *s) { return (double)df_revision(s); }
EMSCRIPTEN_KEEPALIVE int dfb_write(df_store *s, const char *json) {
  df_edit edit = {0};
  bool ok = df_edit_set(&edit, 1, DF_ATTR_DOCUMENT_STATE, df_string_z(json));
  int status = ok ? df_edit_apply(s, &edit) : DF_ERROR;
  df_edit_free(&edit);
  if (status != DF_OK) snprintf(last_error, sizeof(last_error), "%s", df_store_error(s));
  return status;
}
EMSCRIPTEN_KEEPALIVE const char *dfb_read(df_store *s) {
  df_slice encoded, text;
  int status = df_store_get(s, 1, DF_ATTR_DOCUMENT_STATE, &encoded);
  if (status == DF_ABSENT) return "null";
  if (status != DF_OK || df_value_text(encoded, &text) != DF_OK) {
    snprintf(last_error, sizeof(last_error), "Invalid state document"); return NULL;
  }
  char *next = realloc(text_result, text.size+1);
  if (!next) { snprintf(last_error, sizeof(last_error), "Out of memory"); return NULL; }
  text_result = next;
  memcpy(text_result, text.data, text.size); text_result[text.size] = 0;
  return text_result;
}
EMSCRIPTEN_KEEPALIVE void *dfb_connect(void) {
  df_buffer error = {0};
  /* Emscripten replaces this address with Module.websocket.url. */
  void *n = df_network_open("127.0.0.1", 7777, &error);
  if (!n) buffer_error(&error);
  return n;
}
EMSCRIPTEN_KEEPALIVE unsigned dfb_push(void *n, void *s, void *identity) {
  df_buffer error = {0}; unsigned channel = df_network_push(n,s,identity,&error);
  if (!channel) buffer_error(&error); return channel;
}
EMSCRIPTEN_KEEPALIVE unsigned dfb_follow(void *n, df_store *s) {
  df_buffer error = {0}; unsigned channel = df_network_follow(n,s,&error);
  if (!channel) buffer_error(&error); return channel;
}
EMSCRIPTEN_KEEPALIVE void dfb_pump(void *n) { df_network_pump(n,2000); }
EMSCRIPTEN_KEEPALIVE const char *dfb_network_error(void *n) { return df_network_error(n); }
EMSCRIPTEN_KEEPALIVE int dfb_connected(void *n) { return df_network_connected(n); }
EMSCRIPTEN_KEEPALIVE void dfb_disconnect(void *n) { df_network_close(n); }
