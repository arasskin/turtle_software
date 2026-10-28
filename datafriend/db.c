#include "store.h"
#include <math.h>

bool df_encode_value(df_writer *w, df_value v) {
  size_t start = w->size;
  if ((v.size && !v.data) || v.size > 65527)
    return false;
  if (!v.tag)
    df_append(w, v.data, v.size);
  else {
    df_u8(w, v.tag);
    switch (v.tag) {
    case 1:
      if (v.bits > 1)
        return false;
      df_u8(w, (uint8_t)v.bits);
      break;
    case 2:
    case 5:
      df_blob(w, df_span(v.data, v.size));
      break;
    case 4: {
      double d;
      memcpy(&d, &v.bits, 8);
      if (!isfinite(d))
        return false;
      if (d == 0)
        v.bits = 0;
      df_u64(w, v.bits);
      break;
    }
    case 6:
      if (v.bits > UINT32_MAX)
        return false; /* fall through */
    case 3:
      df_u64(w, v.bits);
      break;
    default:
      return false;
    }
  }
  return !w->failed && w->size - start <= 65527;
}
df_store *df_store_open(const df_options *o, df_buffer *error) {
  if (!o || !o->directory) {
    *error = (df_buffer){0};
    df_error(error, "missing store options");
    return NULL;
  }
  const char *name = o->namespace_name ? o->namespace_name : "default";
  return df_open_sized((const uint8_t *)o->directory, strlen(o->directory), (const uint8_t *)name,
                       strlen(name), o->durability, o->map_bytes ? o->map_bytes : (size_t)1 << 30,
                       error);
}
df_status df_store_close(df_store *s, df_buffer *error) {
  if (s && (s->notifying || s->network_owner)) {
    *error = (df_buffer){0};
    df_error(error, "detach store and finish callbacks before close");
    return DF_ERROR;
  }
  return df_close_checked(s, error) ? DF_ERROR : DF_OK;
}
size_t df_store_watch_count(const df_store *s) { return s ? s->watches.size : 0; }
const char *df_store_error(const df_store *s) { return s && s->error ? s->error : ""; }
df_status df_store_get(df_store *s, uint32_t entity, uint32_t attribute, df_slice *out) {
  *out = (df_slice){0};
  if (!s || !df_healthy(s) || !df_attribute_name(attribute))
    return DF_ERROR;
  df_box *b = df_current(s, df_pair_key(entity, attribute));
  if (!b)
    return DF_ABSENT;
  *out = (df_slice){b->data, b->size};
  return DF_OK;
}
df_status df_view_get(df_view *v, uint32_t entity, uint32_t attribute, df_slice *out) {
  *out = (df_slice){0};
  if (!v || v->scope->poisoned || !df_attribute_name(attribute))
    return DF_ERROR;
  df_box *b = df_view_box(v, df_pair_key(entity, attribute));
  if (!b)
    return DF_ABSENT;
  *out = (df_slice){b->data, b->size};
  return DF_OK;
}
df_status df_value_int(df_slice b, int64_t *out) {
  if (!b.data || b.size != 9 || (b.data[0] != 3 && b.data[0] != 6))
    return DF_ERROR;
  uint64_t n = df_integer(df_span(b.data + 1, 8));
  memcpy(out, &n, 8);
  return DF_OK;
}
df_status df_value_bool(df_slice b, bool *out) {
  if (!b.data || b.size != 2 || b.data[0] != 1 || b.data[1] > 1)
    return DF_ERROR;
  *out = b.data[1] != 0;
  return DF_OK;
}
df_status df_value_text(df_slice b, df_slice *out) {
  if (!b.data || b.size < 5 || b.data[0] != 2)
    return DF_ERROR;
  df_reader r = {df_span(b.data + 1, 4), false};
  if (df_read32(&r) != b.size - 5)
    return DF_ERROR;
  *out = (df_slice){b.data + 5, b.size - 5};
  return DF_OK;
}
df_status df_value_real(df_slice b, double *out) {
  if (!b.data || b.size != 9 || b.data[0] != 4)
    return DF_ERROR;
  uint64_t n = df_integer(df_span(b.data + 1, 8));
  double value;
  memcpy(&value, &n, 8);
  if (!isfinite(value))
    return DF_ERROR;
  *out = value;
  return DF_OK;
}
df_status df_value_entity(df_slice b, uint32_t *out) {
  if (!b.data || b.size != 9 || b.data[0] != 6)
    return DF_ERROR;
  uint64_t n = df_integer(df_span(b.data + 1, 8));
  if (n > UINT32_MAX)
    return DF_ERROR;
  *out = (uint32_t)n;
  return DF_OK;
}
df_status df_value_bytes(df_slice b, df_slice *out) {
  if (!b.data || b.size < 5 || b.data[0] != 5)
    return DF_ERROR;
  df_reader r = {df_span(b.data + 1, 4), false};
  if (df_read32(&r) != b.size - 5)
    return DF_ERROR;
  *out = (df_slice){b.data + 5, b.size - 5};
  return DF_OK;
}
static void notify_one(df_store *s, uint64_t id) {
  ct_pair *p = ct_map_get(&s->watches, id);
  if (!p)
    return;
  df_watch *w = (df_watch *)p->value;
  df_watch_fn cb = w->callback;
  void *ctx = w->context;
  df_view *v = w->result;
  df_view_retain(v);
  if (cb)
    cb(ctx, v);
  df_view_release(v);
}
void df_notify(df_store *s, df_bytes bytes) {
  if (!s->watches.size || s->notifying)
    return;
  df_reader r = {bytes, false};
  uint32_t count = df_count(&r, 8);
  if (r.failed || r.data.size != (size_t)count * 8)
    return;
  s->notifying = true;
  for (uint32_t i = 0; i < count; ++i)
    notify_one(s, df_read64(&r));
  s->notifying = false;
}
void df_notify_reset(df_store *s) {
  if (s->notifying)
    return;
  /* Callback removal can move map entries; IDs form a stable traversal. New
   * watches get their own initial notification, never this recovery event. */
  uint64_t last = 0, end = s->next_view;
  s->notifying = true;
  for (;;) {
    uint64_t next = end;
    for (size_t i = 0; i < s->watches.size; ++i) {
      uint64_t id = s->watches.items[i].key;
      if (id > last && id < next)
        next = id;
    }
    if (next == end)
      break;
    df_watch *w = (df_watch *)ct_map_get(&s->watches, next)->value;
    w->result->reset = true;
    notify_one(s, next);
    last = next;
  }
  s->notifying = false;
}
df_watch *df_watch_query(df_store *s, df_query_spec spec, df_watch_fn cb, void *ctx) {
  if (!s)
    return NULL;
  df_watch *w = calloc(1, sizeof(*w));
  if (!df_memory(s, w != NULL))
    return NULL;
  if (!df_subscribe_direct(s, spec, &w->id)) {
    free(w);
    return NULL;
  }
  w->result = ((df_query *)ct_map_get(&s->views, w->id)->value)->result;
  df_view_retain(w->result);
  w->callback = cb;
  w->context = ctx;
  if (!df_memory(s, ct_map_put(&s->watches, w->id, (uintptr_t)w))) {
    df_unsubscribe(s, w->id);
    df_view_release(w->result);
    free(w);
    return NULL;
  }
  bool previous = s->notifying;
  s->notifying = true;
  notify_one(s, w->id);
  s->notifying = previous;
  return w;
}
df_watch *df_watch_entity(df_store *s, uint32_t entity, const uint32_t *fields, size_t count,
                          df_watch_fn cb, void *ctx) {
  if (!count || count > 32 || !fields) {
    df_fail(s, "invalid entity fields");
    return NULL;
  }
  df_match clauses[32];
  for (size_t i = 0; i < count; ++i)
    clauses[i] = (df_match){df_id(entity), fields[i], df_var((int)i), true};
  return df_watch_query(s, (df_query_spec){clauses, count, false}, cb, ctx);
}
void df_watch_remove(df_store *s, df_watch **ptr) {
  df_watch *w = *ptr;
  if (!w)
    return;
  ct_pair *p = ct_map_get(&s->watches, w->id);
  if (!p || (df_watch *)p->value != w)
    return;
  *ptr = NULL;
  ct_map_erase(&s->watches, w->id);
  df_unsubscribe(s, w->id);
  df_view_release(w->result);
  free(w);
}
void df_watches_free(df_store *s) {
  while (s->watches.size) {
    df_watch *w = (df_watch *)s->watches.items[s->watches.size - 1].value;
    df_watch_remove(s, &w);
  }
  ct_map_free(&s->watches);
}
df_view *df_query_once(df_store *s, df_query_spec spec) {
  uint64_t id;
  if (!s || !df_subscribe_direct(s, spec, &id))
    return NULL;
  df_view *live = ((df_query *)ct_map_get(&s->views, id)->value)->result;
  /* A detached result must stay a snapshot even when another identical live
   * query owns the source map. Share immutable boxes, not the mutable map. */
  df_view *v = df_view_new(s->scope);
  bool ok = v && ct_map_reserve(&v->facts, live->facts.size);
  for (size_t i = 0; ok && i < live->facts.size; ++i) {
    ct_pair p = live->facts.items[i];
    ok = ct_map_put(&v->facts, p.key, p.value);
    if (ok)
      df_retain((df_box *)p.value);
  }
  df_unsubscribe(s, id);
  if (!df_memory(s, ok)) {
    df_view_release(v);
    return NULL;
  }
  return v;
}
