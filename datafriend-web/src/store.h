#ifndef DF_STORE_H
#define DF_STORE_H
#include "common.h"
#include "datafriend/db.h"
#include "flush.h"
#include "lmdb.h"
typedef struct df_box {
  size_t refs;
  uint64_t hash;
  size_t size;
  uint8_t data[];
} df_box;
static inline df_bytes df_box_bytes(const df_box *v) { return df_span(v->data, v->size); }
static inline df_box *df_box_new(df_bytes bytes) {
  if (bytes.size > SIZE_MAX - sizeof(df_box))
    return NULL;
  df_box *v = malloc(sizeof(*v) + bytes.size);
  if (!v)
    return NULL;
  v->refs = 1;
  v->hash = df_hash(bytes);
  v->size = bytes.size;
  if (bytes.size)
    memcpy(v->data, bytes.data, bytes.size);
  return v;
}
static inline df_box *df_retain(df_box *v) {
  if (v)
    ++v->refs;
  return v;
}
static inline void df_release(df_box *v) {
  if (v && !--v->refs)
    free(v);
}
static inline bool df_box_equal(const df_box *a, const df_box *b) {
  return a == b || (a && b && a->hash == b->hash && df_equal(df_box_bytes(a), df_box_bytes(b)));
}
static inline void df_boxes_clear(ct_map *m) {
  for (size_t i = 0; i < m->size; ++i)
    df_release((df_box *)m->items[i].value);
  ct_map_clear(m);
}
static inline void df_boxes_free(ct_map *m) {
  df_boxes_clear(m);
  ct_map_free(m);
}
typedef struct df_scope {
  size_t refs;
  bool poisoned;
  size_t name_size;
  char name[];
} df_scope;
struct df_view {
  size_t refs;
  df_scope *scope;
  ct_map facts, entities, attributes;
  bool membership, reset;
};
typedef struct df_values {
  df_box *value;
  ct_map ids;
  struct df_values *next;
} df_values;
typedef struct df_column {
  ct_map entities, values;
} df_column;
typedef struct df_term {
  int variable;
  df_bytes constant;
} df_term;
typedef struct df_clause {
  df_term entity, value;
  uint32_t attribute;
  bool bind_value, projected;
} df_clause;
typedef struct df_entity_group {
  df_term entity;
  uint8_t checks[32], count;
} df_entity_group;
typedef struct df_route df_route;
typedef struct df_query {
  struct df_query *next;
  df_writer key;
  df_clause *clauses;
  df_entity_group *entities;
  uint32_t count, entity_count, projected;
  int predicate;
  bool all, local;
  df_view *result;
  size_t owners;
  df_route *routes[32];
  uint32_t route_count;
  ct_map affected;
  bool scheduled, updated;
} df_query;
struct df_route {
  df_route *next;
  uint64_t key;
  uint32_t attribute, predicate;
  df_box *value;
  ct_map groups;
};
typedef struct df_snapshot df_snapshot;
struct df_store {
  MDB_env *env;
  MDB_dbi db, meta;
  int lock, mode, fd;
  df_scope *scope;
  df_column columns[DF_ATTRIBUTE_COUNT];
  ct_map groups, views, general_routes, fixed_routes, predicate_routes;
  ct_map predicate_attributes[DF_ATTRIBUTE_COUNT];
  ct_map wildcard, journal, affected;
  uint64_t next_view, next_entity, revision, recomputed, journal_floor;
  bool tracking, flushing, notifying;
  ct_map watches;
  void *network_owner;
  df_flush flusher;
  df_snapshot *snapshot;
  const char *error;
#ifdef DATAFRIEND_TESTING
  bool fail_commit;
  pthread_mutex_t test_mutex;
  bool fail_flush;
#endif
};
static inline bool df_fail(df_store *s, const char *message) {
  s->error = message;
  return false;
}
static inline bool df_check(df_store *s, int rc) {
  return rc ? df_fail(s, mdb_strerror(rc)) : true;
}
static inline bool df_memory(df_store *s, bool ok) {
  return ok ? true : df_fail(s, "out of memory");
}
static inline bool df_healthy(df_store *s) {
  return !s->scope->poisoned || df_fail(s, "Datafriend recovery failed; reopen store");
}
static inline df_box *df_current(df_store *s, uint64_t key) {
  size_t a = df_attribute_index((uint32_t)key);
  if (a >= DF_ATTRIBUTE_COUNT)
    return NULL;
  ct_pair *p = ct_map_get(&s->columns[a].entities, key >> 32);
  return p ? (df_box *)p->value : NULL;
}
static inline uint64_t df_pair_key(uint32_t e, uint32_t a) { return (uint64_t)e << 32 | a; }
static inline void df_changes_clear(df_view *v) {
  ct_map_clear(&v->entities);
  ct_map_clear(&v->attributes);
  v->reset = v->membership = false;
}
static inline void df_scope_release(df_scope *s) {
  if (s && !--s->refs)
    free(s);
}
df_view *df_view_new(df_scope *);
void df_view_retain(df_view *);
void df_view_release(df_view *);
bool df_view_encode(df_store *, df_view *, df_writer *);
bool df_subscribe(df_store *, df_bytes, uint64_t *);
bool df_subscribe_direct(df_store *, df_query_spec, uint64_t *);
bool df_encode_value(df_writer *, df_value);
void df_notify(df_store *, df_bytes);
void df_notify_reset(df_store *);
void df_watches_free(df_store *);
void df_unsubscribe(df_store *, uint64_t);
void df_queries_free(df_store *);
bool df_evaluate(df_store *, df_query *, bool, uint32_t, bool);
bool df_route_changes(df_store *, const ct_map *, bool);
void df_routes_reset(df_store *);
df_values *df_values_find(df_column *, df_bytes, uint64_t);
bool df_index(df_store *, uint64_t, df_box *);
void df_indexes_free(df_store *);
bool df_rebuild(df_store *);
bool df_commit(df_store *, ct_map *, const uint64_t *, df_writer *, bool *);
bool df_fact_decode(df_store *, uint64_t, df_bytes, df_bytes *);
bool df_fact_encode(df_store *, uint64_t, df_bytes, df_writer *);
bool df_fingerprint(df_store *, MDB_txn *, uint64_t *);
bool df_sync(df_store *, unsigned, df_bytes, df_writer *);
bool df_sync_apply(df_store *, df_bytes, df_writer *);
bool df_sync_delta(df_store *, uint64_t, df_writer *, bool *);
bool df_sync_apply_delta(df_store *, df_bytes, df_writer *, bool *);
void df_sync_cancel(df_store *);
#endif
