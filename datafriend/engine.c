#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "store.h"
#include "core.h"
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
static MDB_val val(df_bytes b) {
  MDB_val v = {b.size, (void *)b.data};
  return v;
}
static void written(df_store *s) {
  if (s->flushing)
    df_flush_written(&s->flusher);
}
static bool writable(df_store *s) {
  return (!s->notifying || df_fail(s, "cannot edit store from its watch callback")) &&
         df_healthy(s) && (!s->flushing || df_check(s, df_flush_check(&s->flusher)));
}
static int sync_fd(void *ctx) {
  df_store *s = ctx;
#ifdef DATAFRIEND_TESTING
  pthread_mutex_lock(&s->test_mutex);
  bool fail = s->fail_flush;
  pthread_mutex_unlock(&s->test_mutex);
  if (fail)
    return EIO;
#endif
  int rc;
  do {
#ifdef __APPLE__
    rc = fcntl(s->fd, F_FULLFSYNC);
#else
    rc = fsync(s->fd);
#endif
  } while (rc < 0 && errno == EINTR);
  return rc < 0 ? errno : 0;
}
static bool write_counter(df_store *s, MDB_txn *tx, uint64_t counter) {
  uint8_t bytes[8];
  df_store64(bytes, counter);
  MDB_val k = val(df_text("next_entity")), v = val(df_span(bytes, 8));
  return df_check(s, mdb_put(tx, s->meta, &k, &v, 0));
}
static uint64_t fact_hash(uint64_t key, df_bytes bytes) {
  uint8_t k[8];
  df_store64(k, key);
  XXH64_state_t state;
  XXH64_reset(&state, 0);
  XXH64_update(&state, k, 8);
  XXH64_update(&state, bytes.data, bytes.size);
  return XXH64_digest(&state);
}
uint64_t df_hash_bytes(const void *p, size_t n) { return df_hash(df_span(p, n)); }
uint64_t df_hash_record(uint64_t key, const void *p, size_t n) {
  return fact_hash(key, df_span(p, n));
}
bool df_fact_encode(df_store *s, uint64_t key, df_bytes value, df_writer *w) {
  if (value.size > 65527)
    return df_fail(s, "oversized value");
  df_u64(w, fact_hash(key, value));
  df_append(w, value.data, value.size);
  return df_memory(s, !w->failed);
}
bool df_fact_decode(df_store *s, uint64_t key, df_bytes record, df_bytes *out) {
  if (record.size < 8 || record.size > 65535)
    return df_fail(s, "invalid fact record size");
  *out = df_span(record.data + 8, record.size - 8);
  if (fact_hash(key, *out) != df_integer(record))
    return df_fail(s, "fact fingerprint mismatch");
  return true;
}
bool df_fingerprint(df_store *s, MDB_txn *tx, uint64_t *out) {
  MDB_agg aggregate = {0};
  if (!df_check(s, mdb_agg_totals(tx, s->db, &aggregate)))
    return false;
  *out = df_integer(df_span(aggregate.mv_agg_hashes, 8));
  return true;
}
bool df_rebuild(df_store *s) {
  df_indexes_free(s);
  MDB_txn *tx = NULL;
  MDB_cursor *c = NULL;
  bool ok = false;
  if (!df_check(s, mdb_txn_begin(s->env, NULL, MDB_RDONLY, &tx)) ||
      !df_check(s, mdb_cursor_open(tx, s->db, &c)))
    goto done;
  MDB_val k, v;
  int rc;
  while (!(rc = mdb_cursor_get(c, &k, &v, MDB_NEXT))) {
    if (k.mv_size != 8) {
      df_fail(s, "invalid fact key");
      goto done;
    }
    uint64_t key = df_integer(df_span(k.mv_data, k.mv_size));
    if (!df_attribute_name((uint32_t)key)) {
      df_fail(s, "unknown stored attribute");
      goto done;
    }
    if ((key >> 32) >= s->next_entity) {
      df_fail(s, "entity counter behind stored IDs");
      goto done;
    }
    df_bytes bytes;
    if (!df_fact_decode(s, key, df_span(v.mv_data, v.mv_size), &bytes))
      goto done;
    df_box *box = df_box_new(bytes);
    if (!df_memory(s, box != NULL))
      goto done;
    bool indexed = df_index(s, key, box);
    df_release(box);
    if (!indexed)
      goto done;
  }
  ok = df_check(s, rc == MDB_NOTFOUND ? 0 : rc);
done:
  if (c)
    mdb_cursor_close(c);
  if (tx)
    mdb_txn_abort(tx);
  return ok;
}
static bool directories(df_store *s, char *path) {
  for (char *p = path + 1; *p; ++p)
    if (*p == '/') {
      *p = 0;
      int rc = mkdir(path, 0700);
      *p = '/';
      if (rc && errno != EEXIST)
        return df_fail(s, strerror(errno));
    }
  if (mkdir(path, 0700) && errno != EEXIST)
    return df_fail(s, strerror(errno));
  return true;
}
static bool open_store(df_store *s, char *path, size_t map_bytes) {
  MDB_txn *tx = NULL;
  bool ok = false;
  if (!directories(s, path))
    return false;
  size_t n = strlen(path);
  char *lock = malloc(n + 18);
  if (!df_memory(s, lock != NULL))
    return false;
  memcpy(lock, path, n);
  memcpy(lock + n, "/datafriend.owner", 18);
  s->lock = open(lock, O_CREAT | O_RDWR, 0600);
  free(lock);
  if (s->lock < 0 || flock(s->lock, LOCK_EX | LOCK_NB))
    return df_fail(s, "Datafriend directory already owned or inaccessible");
  if (!df_check(s, mdb_env_create(&s->env)) ||
      !df_check(s, mdb_env_set_mapsize(s->env, map_bytes)) ||
      !df_check(s, mdb_env_set_maxdbs(s->env, 2)) ||
      !df_check(s, mdb_env_open(s->env, path, MDB_NOTLS | (s->mode == 1 ? 0 : MDB_NOSYNC), 0600)))
    return false;
  if (s->mode == 2) {
    if (!df_check(s, mdb_env_get_fd(s->env, &s->fd)) ||
        !df_check(s, df_flush_init(&s->flusher, sync_fd, s, 100)))
      return false;
    s->flushing = true;
  }
  if (!df_check(s, mdb_txn_begin(s->env, NULL, 0, &tx)) ||
      !df_check(s, mdb_dbi_open(tx, "meta", MDB_CREATE, &s->meta)))
    goto done;
  const char *codec = "datafriend/3;xor64;ea32;fact-v1;" DATAFRIEND_SCHEMA;
  const char *old_codec =
      "datafriend/"
      "3;xor64;ea32;fact-v1;6105b134821d2173f88730e7bb22129a95388014580dd34caf7ee8d7062ec667";
  MDB_val k = val(df_text("codec")), v = val(df_text(codec)), existing;
  int rc = mdb_get(tx, s->meta, &k, &existing);
  bool fresh = rc == MDB_NOTFOUND;
  if (fresh) {
    if (!df_check(s, mdb_put(tx, s->meta, &k, &v, 0)))
      goto done;
  } else {
    if (!df_check(s, rc))
      goto done;
    df_bytes stored = df_span(existing.mv_data, existing.mv_size);
    if (df_equal(stored, df_text(old_codec))) {
      if (!df_check(s, mdb_put(tx, s->meta, &k, &v, 0)))
        goto done;
    } else if (!df_equal(stored, df_text(codec))) {
      df_fail(s, "incompatible Datafriend codec");
      goto done;
    }
  }
  if (!df_check(s,
                mdb_dbi_open(tx, "facts", MDB_CREATE | MDB_AGG_ENTRIES | MDB_AGG_HASHSUM, &s->db)))
    goto done;
  if (fresh) {
    if (!df_check(s, mdb_set_hash_offset(tx, s->db, 0)))
      goto done;
  } else {
    int offset;
    if (!df_check(s, mdb_get_hash_offset(tx, s->db, &offset)))
      goto done;
    if (offset) {
      df_fail(s, "incompatible hash offset");
      goto done;
    }
  }
  k = val(df_text("namespace"));
  rc = mdb_get(tx, s->meta, &k, &existing);
  v = val(df_span(s->scope->name, s->scope->name_size));
  if (rc == MDB_NOTFOUND) {
    if (!df_check(s, mdb_put(tx, s->meta, &k, &v, 0)))
      goto done;
  } else if (!df_check(s, rc))
    goto done;
  else if (!df_equal(df_span(existing.mv_data, existing.mv_size), df_span(v.mv_data, v.mv_size))) {
    df_fail(s, "store belongs to another namespace");
    goto done;
  }
  k = val(df_text("next_entity"));
  rc = mdb_get(tx, s->meta, &k, &existing);
  if (rc != MDB_NOTFOUND) {
    if (!df_check(s, rc))
      goto done;
    if (existing.mv_size != 8) {
      df_fail(s, "invalid entity counter");
      goto done;
    }
    s->next_entity = df_integer(df_span(existing.mv_data, 8));
  }
  if (s->next_entity < 1 || s->next_entity > UINT64_C(1) + UINT32_MAX) {
    df_fail(s, "invalid entity counter");
    goto done;
  }
  {
    MDB_txn *commit = tx;
    tx = NULL;
    if (!df_check(s, mdb_txn_commit(commit)))
      goto done;
  }
  written(s);
  ok = df_rebuild(s);
done:
  if (tx)
    mdb_txn_abort(tx);
  return ok;
}
void *df_alloc(size_t size) { return malloc(size); }
void df_free(void *p) { free(p); }
int df_attribute_schema(df_buffer *out) {
  *out = (df_buffer){0};
  df_writer w = {0};
  df_append(&w, DATAFRIEND_SCHEMA, sizeof(DATAFRIEND_SCHEMA) - 1);
  return df_reply(&w, out);
}
void *df_open_sized(const uint8_t *path, size_t n, const uint8_t *name, size_t namesize, int mode,
                    size_t mapsize, df_buffer *out) {
  *out = (df_buffer){0};
  if (!path || !n || n > 32768 || memchr(path, 0, n) || !name || !namesize || namesize > 1024 ||
      mode < 0 || mode > 2) {
    df_error(out, "invalid directory, namespace or durability");
    return NULL;
  }
  df_store *s = calloc(1, sizeof(*s));
  char *directory = malloc(n + 1);
  df_scope *scope = calloc(1, sizeof(*scope) + namesize + 1);
  if (!s || !directory || !scope) {
    free(s);
    free(directory);
    free(scope);
    df_error(out, "out of memory");
    return NULL;
  }
  scope->refs = 1;
  scope->name_size = namesize;
  memcpy(scope->name, name, namesize);
  s->scope = scope;
  s->lock = -1;
  s->mode = mode;
  s->next_entity = s->next_view = 1;
#ifdef DATAFRIEND_TESTING
  int rc = pthread_mutex_init(&s->test_mutex, NULL);
  if (rc) {
    free(s);
    free(directory);
    free(scope);
    df_error(out, strerror(rc));
    return NULL;
  }
#endif
  memcpy(directory, path, n);
  directory[n] = 0;
  bool ok = open_store(s, directory, mapsize);
  free(directory);
  if (!ok) {
    df_error(out, s->error);
    df_close(s);
    return NULL;
  }
  return s;
}
void *df_open_namespace(const uint8_t *path, size_t n, const uint8_t *name, size_t namesize,
                        int mode, df_buffer *out) {
  return df_open_sized(path, n, name, namesize, mode, (size_t)1 << 30, out);
}
void *df_open(const uint8_t *path, size_t n, int mode, df_buffer *out) {
  return df_open_namespace(path, n, (const uint8_t *)"default", 7, mode, out);
}
void df_close(void *handle) {
  df_store *s = handle;
  if (!s)
    return;
  df_sync_cancel(s);
  if (s->flushing) {
    int rc = df_flush_stop(&s->flusher);
    if (rc)
      fprintf(stderr, "Datafriend final flush failed: %s\n", strerror(rc));
    df_flush_destroy(&s->flusher);
  }
  df_watches_free(s);
  df_queries_free(s);
  df_indexes_free(s);
  ct_map_free(&s->journal);
  if (s->env)
    mdb_env_close(s->env);
  if (s->lock >= 0) {
    flock(s->lock, LOCK_UN);
    close(s->lock);
  }
  df_scope_release(s->scope);
#ifdef DATAFRIEND_TESTING
  pthread_mutex_destroy(&s->test_mutex);
#endif
  free(s);
}
static bool flush(df_store *s) {
  return df_check(s, s->flushing ? df_flush_barrier(&s->flusher) : mdb_env_sync(s->env, 1));
}
int df_close_checked(void *handle, df_buffer *out) {
  df_store *s = handle;
  *out = (df_buffer){0};
  bool ok = !s || flush(s);
  if (!ok)
    df_error(out, s->error);
  df_close(s);
  return ok ? 0 : 1;
}
uint64_t df_revision(void *p) { return ((df_store *)p)->revision; }
uint64_t df_recomputed(void *p) { return ((df_store *)p)->recomputed; }
void df_namespace(void *p, const uint8_t **bytes, size_t *size) {
  df_store *s = p;
  *bytes = (const uint8_t *)s->scope->name;
  *size = s->scope->name_size;
}
#ifdef DATAFRIEND_TESTING
void df_test_commit_failure(void *p) { ((df_store *)p)->fail_commit = true; }
void df_test_flush_failure(void *p, int fail) {
  df_store *s = p;
  pthread_mutex_lock(&s->test_mutex);
  s->fail_flush = fail != 0;
  pthread_mutex_unlock(&s->test_mutex);
}
uint64_t df_test_flush_attempts(void *p) {
  df_store *s = p;
  return s->flushing ? df_flush_attempts(&s->flusher) : 0;
}
#endif
/* The write transaction remains uncommitted while indexes/results are updated.
 * Any fallible preparation fails before publication; recovery rebuilds existing
 * live handles from the committed database, poisoning them if recovery fails. */
static bool commit(df_store *s, ct_map *changes, const uint64_t *expected, df_writer *out,
                   bool *mismatch, uint64_t reserved) {
  if (mismatch)
    *mismatch = false;
  if (!writable(s))
    return false;
  s->recomputed = 0;
  MDB_txn *tx = NULL;
  df_writer encoded = {0};
  bool ok = false, mutated = false;
  uint64_t counter = reserved > s->next_entity ? reserved : s->next_entity;
  if (!df_memory(s, df_reserve(out, 4 + 8 * s->views.size)) ||
      !df_check(s, mdb_txn_begin(s->env, NULL, 0, &tx)))
    goto done;
  for (size_t i = 0; i < changes->size;) {
    ct_pair p = changes->items[i];
    df_box *next = (df_box *)p.value, *old = df_current(s, p.key);
    if (!df_attribute_name((uint32_t)p.key)) {
      df_fail(s, "unknown shared attribute");
      goto done;
    }
    if (df_box_equal(old, next)) {
      df_release(next);
      ct_map_erase(changes, p.key);
      continue;
    }
    uint8_t kb[8];
    df_store64(kb, p.key);
    MDB_val k = val(df_span(kb, 8));
    if (next) {
      encoded.size = 0;
      if (!df_fact_encode(s, p.key, df_box_bytes(next), &encoded))
        goto done;
      MDB_val v = val(df_written(&encoded));
      if (!df_check(s, mdb_put(tx, s->db, &k, &v, 0)))
        goto done;
    } else if (!df_check(s, mdb_del(tx, s->db, &k, NULL)))
      goto done;
    if (counter < (p.key >> 32) + 1)
      counter = (p.key >> 32) + 1;
    ++i;
  }
  if (counter != s->next_entity && !write_counter(s, tx, counter))
    goto done;
  if (expected) {
    uint64_t hash;
    if (!df_fingerprint(s, tx, &hash))
      goto done;
    if (hash != *expected) {
      if (mismatch)
        *mismatch = true;
      ok = true;
      goto done;
    }
  }
  if (!changes->size && counter == s->next_entity) {
    df_u32(out, 0);
    ok = true;
    goto done;
  }
  df_routes_reset(s);
  if (!df_route_changes(s, changes, false))
    goto done;
  mutated = true;
  for (size_t i = 0; i < changes->size; ++i)
    if (!df_index(s, changes->items[i].key, (df_box *)changes->items[i].value))
      goto done;
  if (!df_route_changes(s, changes, true))
    goto done;
  uint64_t recomputed = 0;
  for (size_t i = 0; i < s->affected.size; ++i) {
    df_query *q = (df_query *)(uintptr_t)s->affected.items[i].key;
    ++recomputed;
    df_changes_clear(q->result);
    if (q->local) {
      for (size_t j = 0; j < q->affected.size; ++j)
        if (!df_evaluate(s, q, true, (uint32_t)q->affected.items[j].key, true))
          goto done;
    } else if (!df_evaluate(s, q, false, 0, true))
      goto done;
  }
  /* Emit sorted subscription IDs without allocating after the commit. */
  uint32_t count = 0;
  df_u32(out, 0);
  for (size_t i = 0; i < s->views.size; ++i) {
    ct_pair p = s->views.items[i];
    if (((df_query *)p.value)->scheduled && ((df_query *)p.value)->updated) {
      df_u64(out, p.key);
      ++count;
    }
  }
  /* Dense view entries are usually insertion ordered; deletions can move them. */
  for (size_t i = 1; i < count; ++i) {
    uint8_t *at = out->data + 4 + 8 * i;
    uint64_t key = df_integer(df_span(at, 8));
    size_t j = i;
    while (j && df_integer(df_span(out->data + 4 + 8 * (j - 1), 8)) > key) {
      memcpy(out->data + 4 + 8 * j, out->data + 4 + 8 * (j - 1), 8);
      --j;
    }
    df_store64(out->data + 4 + 8 * j, key);
  }
  for (unsigned i = 0; i < 4; ++i)
    out->data[i] = (uint8_t)(count >> (24 - 8 * i));
#ifdef DATAFRIEND_TESTING
  if (s->fail_commit) {
    s->fail_commit = false;
    df_fail(s, "injected commit failure");
    goto done;
  }
#endif
  {
    MDB_txn *commit = tx;
    tx = NULL;
    if (!df_check(s, mdb_txn_commit(commit)))
      goto done;
  }
  written(s);
  s->next_entity = counter;
  ++s->revision;
  s->recomputed = recomputed;
  if (s->tracking) {
    for (size_t i = 0; i < changes->size; ++i)
      if (!ct_map_put(&s->journal, changes->items[i].key, (uintptr_t)s->revision) ||
          s->journal.size > 4096) {
        ct_map_clear(&s->journal);
        s->journal_floor = s->revision;
        break;
      }
  }
  ok = true;
done:
  if (tx)
    mdb_txn_abort(tx);
  df_writer_free(&encoded);
  if (!ok && mutated) {
    const char *failure = s->error;
    bool recovered = df_rebuild(s);
    for (size_t i = 0; recovered && i < s->groups.size; ++i)
      for (df_query *q = (df_query *)s->groups.items[i].value; q; q = q->next) {
        df_boxes_clear(&q->result->facts);
        df_changes_clear(q->result);
        q->result->reset = true;
        if (!df_evaluate(s, q, false, 0, false)) {
          recovered = false;
          break;
        }
      }
    if (!recovered)
      s->scope->poisoned = true;
    s->error = failure;
  }
  return ok;
}
bool df_commit(df_store *s, ct_map *changes, const uint64_t *expected, df_writer *out,
               bool *mismatch) {
  return commit(s, changes, expected, out, mismatch, 0);
}
static bool change(df_store *s, ct_map *changes, uint64_t key, df_bytes bytes, bool present) {
  df_box *box = present ? df_box_new(bytes) : NULL;
  if (present && !df_memory(s, box != NULL))
    return false;
  ct_pair *old = ct_map_get(changes, key);
  df_box *previous = old ? (df_box *)old->value : NULL;
  if (!df_memory(s, ct_map_put(changes, key, (uintptr_t)box))) {
    df_release(box);
    return false;
  }
  df_release(previous);
  return true;
}
static bool triples(df_store *s, df_reader *r, ct_map *changes, bool add) {
  uint32_t count = df_count(r, 16);
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t id = df_read64(r);
    df_bytes name = df_read_blob(r), value = df_read_blob(r);
    uint32_t attr = df_attribute_id(name.data, name.size);
    if (r->failed)
      return df_fail(s, "truncated triples");
    if (id > UINT32_MAX || !attr || value.size > 65527)
      return df_fail(s, "invalid triple");
    uint64_t key = df_pair_key((uint32_t)id, attr);
    if (add) {
      if (!change(s, changes, key, value, true))
        return false;
    } else {
      ct_pair *pending = ct_map_get(changes, key);
      df_box *old = pending ? (df_box *)pending->value : df_current(s, key);
      if (old && df_equal(df_box_bytes(old), value) && !change(s, changes, key, value, false))
        return false;
    }
  }
  return !r->failed || df_fail(s, "truncated triples");
}
static bool decode_entity(df_store *s, uint64_t id, df_bytes input, ct_map *changes) {
  if (id > UINT32_MAX || input.size > 64 * 1024 * 1024)
    return df_fail(s, "invalid entity record");
  df_reader r = {input, false};
  uint64_t expected = df_read64(&r);
  if (r.failed || fact_hash(id, r.data) != expected)
    return df_fail(s, "entity fingerprint mismatch");
  if (df_read8(&r) != 1)
    return df_fail(s, "unsupported entity codec");
  uint32_t n = df_count(&r, 8);
  df_bytes previous = {0};
  for (uint32_t i = 0; i < n; ++i) {
    df_bytes a = df_read_blob(&r), v = df_read_blob(&r);
    uint32_t attr = df_attribute_id(a.data, a.size);
    if (!attr || v.size > 65527 || (i && df_compare(a, previous) <= 0))
      return df_fail(s, "noncanonical entity attributes");
    previous = a;
    if (!change(s, changes, df_pair_key((uint32_t)id, attr), v, true))
      return false;
  }
  return df_end(&r) || df_fail(s, "invalid entity record");
}
static bool imports(df_store *s, df_reader *r, ct_map *changes, bool entities) {
  uint32_t count = df_count(r, 9);
  ct_map seen = {0};
  bool ok = false;
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t key = df_read64(r);
    unsigned present = df_read8(r);
    df_bytes value = {0};
    if (present)
      value = df_read_blob(r);
    if (r->failed || present > 1 || ct_map_get(&seen, key)) {
      df_fail(s, "invalid or duplicate imported record");
      goto done;
    }
    if (!df_memory(s, ct_map_put(&seen, key, 0)))
      goto done;
    if (entities) {
      if (key > UINT32_MAX) {
        df_fail(s, "entity ID exceeds uint32 range");
        goto done;
      }
      for (size_t a = 0; a < DF_ATTRIBUTE_COUNT; ++a) {
        uint64_t fact = df_pair_key((uint32_t)key, df_attributes[a].id);
        if (df_current(s, fact) && !change(s, changes, fact, df_span(NULL, 0), false))
          goto done;
      }
      if (present && !decode_entity(s, key, value, changes))
        goto done;
    } else {
      df_bytes bytes = {0};
      if (present && !df_fact_decode(s, key, value, &bytes))
        goto done;
      if (!change(s, changes, key, bytes, present != 0))
        goto done;
    }
  }
  ok = !r->failed;
done:
  ct_map_free(&seen);
  return ok;
}
static int attr_compare(const void *a, const void *b) {
  const df_attribute_definition *const *x = a, *const *y = b;
  return strcmp((*x)->name, (*y)->name);
}
static bool export_entity(df_store *s, uint32_t id, df_writer *out) {
  const df_attribute_definition *attrs[DF_ATTRIBUTE_COUNT];
  size_t n = 0;
  for (size_t i = 0; i < DF_ATTRIBUTE_COUNT; ++i)
    if (df_current(s, df_pair_key(id, df_attributes[i].id)))
      attrs[n++] = df_attributes + i;
  qsort(attrs, n, sizeof(*attrs), attr_compare);
  df_writer body = {0};
  df_u8(&body, 1);
  df_u32(&body, (uint32_t)n);
  for (size_t i = 0; i < n; ++i) {
    df_blob(&body, df_text(attrs[i]->name));
    df_blob(&body, df_box_bytes(df_current(s, df_pair_key(id, attrs[i]->id))));
  }
  if (!df_memory(s, !body.failed)) {
    df_writer_free(&body);
    return false;
  }
  df_u64(out, fact_hash(id, df_written(&body)));
  df_append(out, body.data, body.size);
  df_writer_free(&body);
  return df_memory(s, !out->failed);
}
static bool exports(df_store *s, bool entities, df_writer *out) {
  MDB_txn *tx = NULL;
  MDB_cursor *cursor = NULL;
  bool ok = false;
  uint32_t count = 0;
  df_writer record = {0};
  if (!df_healthy(s) || !df_check(s, mdb_txn_begin(s->env, NULL, MDB_RDONLY, &tx)) ||
      !df_check(s, mdb_cursor_open(tx, s->db, &cursor)))
    goto done;
  df_u32(out, 0);
  MDB_val k, v;
  int rc;
  uint64_t previous = UINT64_MAX;
  while (!(rc = mdb_cursor_get(cursor, &k, &v, MDB_NEXT))) {
    if (k.mv_size != 8) {
      df_fail(s, "invalid fact key");
      goto done;
    }
    uint64_t key = df_integer(df_span(k.mv_data, 8));
    if (entities) {
      key >>= 32;
      if (key == previous)
        continue;
      previous = key;
      record.size = 0;
      if (!export_entity(s, (uint32_t)key, &record))
        goto done;
      df_u64(out, key);
      df_blob(out, df_written(&record));
    } else {
      df_u64(out, key);
      df_blob(out, df_span(v.mv_data, v.mv_size));
    }
    ++count;
  }
  if (!df_check(s, rc == MDB_NOTFOUND ? 0 : rc) || !df_memory(s, !out->failed))
    goto done;
  for (unsigned i = 0; i < 4; ++i)
    out->data[i] = (uint8_t)(count >> (24 - 8 * i));
  ok = true;
done:
  df_writer_free(&record);
  if (cursor)
    mdb_cursor_close(cursor);
  if (tx)
    mdb_txn_abort(tx);
  return ok;
}
static bool view_exists(df_store *s, uint64_t id) {
  return ct_map_get(&s->views, id) != NULL || df_fail(s, "unknown view");
}
int df_call(void *handle, const uint8_t *input, size_t size, df_buffer *out) {
  df_store *s = handle;
  *out = (df_buffer){0};
  if (!s || !input || size > 64 * 1024 * 1024) {
    df_error(out, "invalid request");
    return 1;
  }
  s->error = NULL;
  df_reader r = {df_span(input, size), false};
  df_writer w = {0};
  ct_map changes = {0}, adds = {0};
  bool ok = false;
  unsigned op = 0;
  if (df_read8(&r) != 1) {
    df_fail(s, "unsupported API version");
    goto done;
  }
  op = df_read8(&r);
  if (!df_healthy(s) && op != 11)
    goto done;
  switch (op) {
  case 1: {
    uint64_t id;
    if (!df_memory(s, df_reserve(&w, 8)) || !df_subscribe(s, r.data, &id))
      goto done;
    df_u64(&w, id);
    ok = true;
    break;
  }
  case 2: {
    uint64_t id = df_read64(&r);
    if (!df_end(&r) || !view_exists(s, id))
      goto done;
    ok = df_view_encode(s, ((df_query *)ct_map_get(&s->views, id)->value)->result, &w);
    break;
  }
  case 3:
  case 10: {
    if (op == 3) {
      uint64_t id = df_read64(&r);
      if (!view_exists(s, id))
        goto done;
    }
    if (!triples(s, &r, &changes, false) || !triples(s, &r, &changes, true) || !df_end(&r))
      goto done;
    ok = df_commit(s, &changes, NULL, &w, NULL);
    break;
  }
  case 4: {
    uint64_t id = df_read64(&r);
    if (!df_end(&r))
      goto done;
    df_unsubscribe(s, id);
    ok = true;
    break;
  }
  case 5: {
    if (!df_end(&r) || !writable(s) || !df_memory(s, df_reserve(&w, 8)))
      goto done;
    if (s->next_entity > UINT32_MAX) {
      df_fail(s, "entity IDs exhausted in namespace");
      goto done;
    }
    MDB_txn *tx = NULL;
    if (!df_check(s, mdb_txn_begin(s->env, NULL, 0, &tx)))
      goto done;
    if (!write_counter(s, tx, s->next_entity + 1)) {
      mdb_txn_abort(tx);
      goto done;
    }
    if (!df_check(s, mdb_txn_commit(tx)))
      goto done;
    written(s);
    df_u64(&w, s->next_entity++);
    ok = true;
    break;
  }
  case 6:
  case 12:
    if (df_end(&r))
      ok = exports(s, op == 6, &w);
    break;
  case 7:
  case 13:
    if (imports(s, &r, &changes, op == 7) && df_end(&r))
      ok = df_commit(s, &changes, NULL, &w, NULL);
    break;
  case 8: {
    uint32_t n = df_count(&r, 16);
    for (uint32_t i = 0; i < n; ++i) {
      uint64_t id = df_read64(&r);
      if (!view_exists(s, id) || !triples(s, &r, &changes, false) || !triples(s, &r, &adds, true))
        goto done;
    }
    if (!df_end(&r))
      goto done;
    for (size_t i = 0; i < adds.size; ++i) {
      ct_pair p = adds.items[i];
      if (!change(s, &changes, p.key, df_box_bytes((df_box *)p.value), true))
        goto done;
    }
    ok = df_commit(s, &changes, NULL, &w, NULL);
    break;
  }
  case 9: {
    uint64_t id = df_read64(&r), entity = df_read64(&r);
    df_bytes attr = df_read_blob(&r);
    if (!df_end(&r) || !view_exists(s, id) || entity > UINT32_MAX)
      goto done;
    uint32_t a = df_attribute_id(attr.data, attr.size);
    df_view *v = ((df_query *)ct_map_get(&s->views, id)->value)->result;
    ct_pair *p = ct_map_get(&v->facts, df_pair_key((uint32_t)entity, a));
    df_u8(&w, p != NULL);
    if (p) {
      df_u64(&w, entity);
      df_blob(&w, attr);
      df_blob(&w, df_box_bytes((df_box *)p->value));
    }
    ok = true;
    break;
  }
  case 11:
    if (df_end(&r))
      ok = flush(s);
    break;
  case 14: {
    unsigned command = df_read8(&r);
    df_bytes packet = df_read_blob(&r);
    if (df_end(&r))
      ok = df_sync(s, command, packet, &w);
    break;
  }
  case 15: {
    df_bytes packet = df_read_blob(&r);
    if (df_end(&r))
      ok = df_sync_apply(s, packet, &w);
    break;
  }
  default:
    df_fail(s, "unknown operation");
    break;
  }
done:
  df_boxes_free(&changes);
  df_boxes_free(&adds);
  if (ok) {
    if (op == 3 || op == 7 || op == 8 || op == 10 || op == 13 || op == 15)
      df_notify(s, df_written(&w));
    return df_reply(&w, out);
  }
  const char *failure = s->error;
  if (op == 3 || op == 7 || op == 8 || op == 10 || op == 13 || op == 15)
    df_notify_reset(s);
  s->error = failure;
  df_writer_free(&w);
  df_error(out, s->error ? s->error : "invalid request packet");
  return 1;
}

/* Native edits carry numeric keys and one owned copy of encoded values. */
typedef struct edit_item {
  uint32_t entity, attribute;
  size_t offset, size;
  unsigned op;
} edit_item;
enum { EDIT_SET, EDIT_UNSET, EDIT_RETRACT, EDIT_DELETE };
static bool edit_append(df_edit *e, uint32_t entity, uint32_t attribute, df_value value,
                        unsigned op) {
  if (e->failed)
    return false;
  if ((op != EDIT_DELETE && !df_attribute_name(attribute)) ||
      !ct_array_reserve(&e->items, &e->capacity, e->count + 1, sizeof(edit_item)))
    goto fail;
  size_t offset = e->size;
  if (op == EDIT_SET || op == EDIT_RETRACT) {
    df_writer w = {e->data, e->size, e->bytes_capacity, false};
    bool ok = df_encode_value(&w, value);
    e->data = w.data;
    e->size = w.size;
    e->bytes_capacity = w.capacity;
    if (!ok)
      goto fail;
  }
  ((edit_item *)e->items)[e->count++] =
      (edit_item){entity, attribute, offset, e->size - offset, op};
  return true;
fail:
  e->failed = true;
  return false;
}
bool df_edit_set(df_edit *e, uint32_t id, uint32_t a, df_value v) {
  return edit_append(e, id, a, v, EDIT_SET);
}
bool df_edit_unset(df_edit *e, uint32_t id, uint32_t a) {
  return edit_append(e, id, a, df_encoded(NULL, 0), EDIT_UNSET);
}
bool df_edit_retract(df_edit *e, uint32_t id, uint32_t a, df_value v) {
  return edit_append(e, id, a, v, EDIT_RETRACT);
}
bool df_edit_delete(df_edit *e, uint32_t id) {
  return edit_append(e, id, 0, df_encoded(NULL, 0), EDIT_DELETE);
}
uint32_t df_edit_create(df_edit *e, df_store *s) {
  if (!s || e->failed || (e->store && e->store != s) || !writable(s))
    goto fail;
  if (!e->store) {
    e->store = s;
    e->first_entity = e->next_entity = s->next_entity;
  }
  if (e->next_entity > UINT32_MAX)
    goto fail;
  return (uint32_t)e->next_entity++;
fail:
  e->failed = true;
  return 0;
}
void df_edit_clear(df_edit *e) {
  e->count = e->size = 0;
  e->failed = false;
  e->store = NULL;
  e->first_entity = e->next_entity = 0;
}
void df_edit_free(df_edit *e) {
  free(e->items);
  free(e->data);
  memset(e, 0, sizeof(*e));
}
df_status df_edit_apply(df_store *s, df_edit *e) {
  ct_map changes = {0};
  df_writer out = {0};
  bool ok = false;
  if (!s) {
    df_edit_clear(e);
    return DF_ERROR;
  }
  s->error = NULL;
  if (!writable(s))
    goto done;
  if (e->failed) {
    df_fail(s, "invalid edit or allocation failure");
    goto done;
  }
  if (e->store && (e->store != s || e->first_entity != s->next_entity)) {
    df_fail(s, "entity allocation changed while building batch");
    goto done;
  }
  for (size_t i = 0; i < e->count; ++i) {
    edit_item item = ((edit_item *)e->items)[i];
    uint64_t key = df_pair_key(item.entity, item.attribute);
    df_bytes bytes = df_span(e->data ? e->data + item.offset : NULL, item.size);
    if (item.op == EDIT_DELETE) {
      for (size_t a = 0; a < DF_ATTRIBUTE_COUNT; ++a) {
        key = df_pair_key(item.entity, df_attributes[a].id);
        ct_pair *p = ct_map_get(&changes, key);
        if ((p ? p->value != 0 : df_current(s, key) != NULL) &&
            !change(s, &changes, key, df_span(NULL, 0), false))
          goto done;
      }
    } else if (item.op == EDIT_RETRACT) {
      ct_pair *p = ct_map_get(&changes, key);
      df_box *old = p ? (df_box *)p->value : df_current(s, key);
      if (old && df_equal(df_box_bytes(old), bytes) && !change(s, &changes, key, bytes, false))
        goto done;
    } else if (!change(s, &changes, key, bytes, item.op == EDIT_SET))
      goto done;
  }
  ok = commit(s, &changes, NULL, &out, NULL, e->next_entity);
done:
  df_boxes_free(&changes);
  df_edit_clear(e);
  const char *failure = s->error;
  if (ok)
    df_notify(s, df_written(&out));
  else
    df_notify_reset(s);
  if (!ok)
    s->error = failure;
  df_writer_free(&out);
  return ok ? DF_OK : DF_ERROR;
}
df_status df_store_flush(df_store *s) { return s && flush(s) ? DF_OK : DF_ERROR; }
