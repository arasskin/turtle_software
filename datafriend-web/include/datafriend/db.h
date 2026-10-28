#ifndef DATAFRIEND_DB_H
#define DATAFRIEND_DB_H
#include "core.h"
#include "schema.h"
#include <stdbool.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Native C API. Serialize access on one application thread. */
typedef struct df_store df_store;
typedef struct df_view df_view;
typedef struct df_watch df_watch;
typedef enum df_status { DF_OK = 0, DF_ABSENT = 1, DF_ERROR = -1 } df_status;
typedef enum df_durability { DF_BUFFERED = 0, DF_SYNC = 1, DF_ASYNC = 2 } df_durability;
typedef struct df_options {
  const char *directory, *namespace_name;
  df_durability durability;
  size_t map_bytes; /* Zero selects 1 GiB of virtual address space. */
} df_options;
/* Bytes/text in values are borrowed. Edit builders copy them on append. Tag 0
 * is already encoded bytes, preserving opaque application codecs. */
typedef struct df_value {
  uint8_t tag;
  uint64_t bits;
  const void *data;
  size_t size;
} df_value;
static inline df_value df_int(int64_t n) {
  df_value v = {3, (uint64_t)n, NULL, 0};
  return v;
}
static inline df_value df_bool(bool n) {
  df_value v = {1, n, NULL, 0};
  return v;
}
static inline df_value df_entity(uint32_t n) {
  df_value v = {6, n, NULL, 0};
  return v;
}
static inline df_value df_string(const char *s, size_t n) {
  df_value v = {2, 0, s, n};
  return v;
}
static inline df_value df_string_z(const char *s) { return df_string(s, strlen(s)); }
static inline df_value df_encoded(const void *p, size_t n) {
  df_value v = {0, 0, p, n};
  return v;
}
static inline df_value df_bytes_value(const void *p, size_t n) {
  df_value v = {5, 0, p, n};
  return v;
}
static inline df_value df_real(double n) {
  df_value v = {4, 0, NULL, 0};
  if (n == 0)
    n = 0;
  memcpy(&v.bits, &n, 8);
  return v;
}
typedef struct df_match_term {
  int variable;
  df_value value;
} df_match_term;
static inline df_match_term df_var(int n) {
  df_match_term t = {n, {0, 0, NULL, 0}};
  return t;
}
static inline df_match_term df_const(df_value v) {
  df_match_term t = {-1, v};
  return t;
}
static inline df_match_term df_id(uint32_t n) { return df_const(df_entity(n)); }
typedef struct df_match {
  df_match_term entity;
  uint32_t attribute;
  df_match_term value;
  bool project;
} df_match;
typedef struct df_query_spec {
  const df_match *clauses;
  size_t count;
  bool all;
} df_query_spec;
typedef struct df_slice {
  const uint8_t *data;
  size_t size;
} df_slice;
/* Init to zero. Reusable storage: apply/clear retains capacity; free releases it.
 * Create reserves IDs within this batch; concurrent ID allocation invalidates
 * its apply. The counter and fields commit together. Failed apply clears the
 * batch but never commits a subset. */
typedef struct df_edit {
  void *items;
  size_t count, capacity;
  uint8_t *data;
  size_t size, bytes_capacity;
  df_store *store;
  uint64_t first_entity, next_entity;
  bool failed;
} df_edit;
typedef void (*df_watch_fn)(void *context, df_view *result);
/* Callback after initial evaluation and each changed commit. Callbacks may
 * add/remove watches and edit OTHER stores; no same-store edits/close or
 * recursive network pump. Watch owns its result; retain to outlive the watch. */
struct df_watch {
  uint64_t id;
  df_view *result;
  df_watch_fn callback;
  void *context;
};
DF_API df_store *df_store_open(const df_options *options, df_buffer *error);
/* Consumes unless still attached to a network or called from a callback. */
DF_API df_status df_store_close(df_store *store, df_buffer *error);
DF_API df_status df_store_flush(df_store *store);
DF_API size_t df_store_watch_count(const df_store *store);
DF_API const char *df_store_error(const df_store *store);
DF_API df_status df_store_get(df_store *store, uint32_t entity, uint32_t attribute, df_slice *out);
DF_API bool df_edit_set(df_edit *edit, uint32_t entity, uint32_t attribute, df_value value);
DF_API bool df_edit_unset(df_edit *edit, uint32_t entity, uint32_t attribute);
DF_API bool df_edit_retract(df_edit *edit, uint32_t entity, uint32_t attribute, df_value expected);
DF_API bool df_edit_delete(df_edit *edit, uint32_t entity);
DF_API uint32_t df_edit_create(df_edit *edit, df_store *store);
DF_API df_status df_edit_apply(df_store *store, df_edit *edit);
DF_API void df_edit_clear(df_edit *edit);
DF_API void df_edit_free(df_edit *edit);
DF_API df_watch *df_watch_query(df_store *store, df_query_spec query, df_watch_fn callback,
                                void *context);
/* Required-field projection: matches when all requested fields exist. */
DF_API df_watch *df_watch_entity(df_store *store, uint32_t entity, const uint32_t *attributes,
                                 size_t count, df_watch_fn callback, void *context);
DF_API void df_watch_remove(df_store *store, df_watch **watch);
/* Evaluates once, with no live subscription. Caller releases the result. */
DF_API df_view *df_query_once(df_store *store, df_query_spec query);
DF_API void df_view_retain(df_view *view);
DF_API void df_view_release(df_view *view);
DF_API df_status df_view_get(df_view *view, uint32_t entity, uint32_t attribute, df_slice *out);
DF_API df_status df_value_int(df_slice encoded, int64_t *out);
DF_API df_status df_value_bool(df_slice encoded, bool *out);
DF_API df_status df_value_real(df_slice encoded, double *out);
DF_API df_status df_value_entity(df_slice encoded, uint32_t *out);
DF_API df_status df_value_bytes(df_slice encoded, df_slice *out);
DF_API df_status df_value_text(df_slice encoded, df_slice *out);
/* Borrowed reads expire at mutation/release. Keep scratch across enumerations;
 * returned ids are owned by this scratch, valid until its next use/free. */
typedef struct df_entities {
  uint32_t *data;
  size_t count, capacity;
  void *scratch;
  size_t scratch_capacity;
} df_entities;
DF_API df_status df_view_entities(df_view *view, uint32_t order, uint32_t secondary,
                                  df_entities *out);
DF_API void df_entities_free(df_entities *entities);
#ifdef __cplusplus
}
#endif
#endif
