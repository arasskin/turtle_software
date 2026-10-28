#include "store.h"
#include "datafriend/core.h"
static bool valid(df_view *v, df_buffer *out) {
  *out = (df_buffer){0};
  if (!v || v->scope->poisoned) {
    df_error(out, "Datafriend recovery failed or result closed");
    return false;
  }
  return true;
}
void *df_result(void *handle, uint64_t id, df_buffer *out) {
  df_store *s = handle;
  *out = (df_buffer){0};
  if (!s || !df_healthy(s)) {
    df_error(out, "closed or poisoned engine");
    return NULL;
  }
  ct_pair *p = ct_map_get(&s->views, id);
  if (!p) {
    df_error(out, "unknown view");
    return NULL;
  }
  df_view *v = ((df_query *)p->value)->result;
  df_view_retain(v);
  return v;
}
void df_view_keep(void *p) { df_view_retain(p); }
void df_result_release(void *p) { df_view_release(p); }
int df_result_same(void *a, void *b) { return a == b; }
int df_view_valid(void *p) {
  df_view *v = p;
  return v && !v->scope->poisoned;
}
size_t df_view_size(void *p) { return ((df_view *)p)->facts.size; }
void *df_view_empty(const uint8_t *name, size_t size) {
  if (size > 1024 || (!name && size))
    return NULL;
  df_scope *s = calloc(1, sizeof(*s) + size + 1);
  if (!s)
    return NULL;
  s->refs = 1;
  s->name_size = size;
  if (size)
    memcpy(s->name, name, size);
  df_view *v = df_view_new(s);
  df_scope_release(s);
  return v;
}
void df_view_namespace(void *p, const uint8_t **bytes, size_t *size) {
  df_scope *s = ((df_view *)p)->scope;
  *bytes = (const uint8_t *)s->name;
  *size = s->name_size;
}
void *df_view_box(void *p, uint64_t key) {
  ct_pair *entry = ct_map_get(&((df_view *)p)->facts, key);
  return entry ? (void *)entry->value : NULL;
}
void *df_view_at(void *p, size_t index, uint64_t *key) {
  df_view *v = p;
  if (index >= v->facts.size)
    return NULL;
  ct_pair pair = v->facts.items[index];
  *key = pair.key;
  return (void *)pair.value;
}
void df_box_keep(void *p) { df_retain(p); }
void df_box_drop(void *p) { df_release(p); }
void df_box_value(void *p, const uint8_t **bytes, size_t *size, uint64_t *hash) {
  df_box *v = p;
  *bytes = v->data;
  *size = v->size;
  *hash = v->hash;
}
#ifdef DATAFRIEND_TESTING
void df_test_poison_view(void *p) { ((df_view *)p)->scope->poisoned = true; }
#endif
int df_result_info(void *p, df_buffer *out) {
  df_view *v = p;
  if (!valid(v, out))
    return 1;
  df_writer w = {0};
  df_u64(&w, v->facts.size);
  return df_reply(&w, out);
}
int df_result_list(void *p, df_buffer *out) {
  df_view *v = p;
  if (!valid(v, out))
    return 1;
  df_writer w = {0};
  df_store dummy = {0};
  df_view_encode(&dummy, v, &w);
  return df_reply(&w, out);
}
int df_result_changes(void *p, df_buffer *out) {
  df_view *v = p;
  if (!valid(v, out))
    return 1;
  df_writer w = {0};
  df_u8(&w, v->reset);
  df_u8(&w, v->membership);
  df_u32(&w, (uint32_t)v->entities.size);
  for (size_t i = 0; i < v->entities.size; ++i)
    df_u32(&w, (uint32_t)v->entities.items[i].key);
  df_u32(&w, (uint32_t)v->attributes.size);
  for (size_t i = 0; i < v->attributes.size; ++i)
    df_blob(&w, df_text(df_attribute_name((uint32_t)v->attributes.items[i].key)));
  return df_reply(&w, out);
}
uint32_t df_result_attribute(void *p, const uint8_t *name, size_t size) {
  (void)p;
  if (!name && size)
    return 0;
  return df_attribute_id(name, size);
}
int df_result_value(void *p, uint32_t entity, uint32_t attribute, df_buffer *out) {
  *out = (df_buffer){0};
  if (!df_view_valid(p))
    return -1;
  df_box *v = df_view_box(p, df_pair_key(entity, attribute));
  if (!v)
    return 1;
  out->data = v->data;
  out->size = v->size;
  return 0;
}
int64_t df_result_find(void *p, uint32_t attribute, const uint8_t *bytes, size_t size) {
  if (!df_view_valid(p) || (!bytes && size))
    return -2;
  df_view *v = p;
  for (size_t i = 0; i < v->facts.size; ++i) {
    ct_pair pair = v->facts.items[i];
    if ((uint32_t)pair.key == attribute &&
        df_equal(df_box_bytes((df_box *)pair.value), df_span(bytes, size)))
      return (int64_t)(pair.key >> 32);
  }
  return -1;
}
int64_t df_result_count_value(void *p, uint32_t attribute, const uint8_t *bytes, size_t size) {
  if (!df_view_valid(p) || (!bytes && size))
    return -1;
  df_view *v = p;
  int64_t n = 0;
  for (size_t i = 0; i < v->facts.size; ++i) {
    ct_pair pair = v->facts.items[i];
    if ((uint32_t)pair.key == attribute &&
        df_equal(df_box_bytes((df_box *)pair.value), df_span(bytes, size)))
      ++n;
  }
  return n;
}
int df_result_attributes(void *p, uint32_t entity, df_buffer *out) {
  df_view *v = p;
  if (!valid(v, out))
    return 1;
  df_writer w = {0};
  uint32_t n = 0;
  for (size_t i = 0; i < v->facts.size; ++i)
    if (v->facts.items[i].key >> 32 == entity)
      ++n;
  df_u32(&w, n);
  for (size_t i = 0; i < v->facts.size; ++i)
    if (v->facts.items[i].key >> 32 == entity)
      df_blob(&w, df_text(df_attribute_name((uint32_t)v->facts.items[i].key)));
  return df_reply(&w, out);
}
typedef struct sort_key {
  uint32_t id;
  bool missing[2];
  int64_t value[2];
} sort_key;
static int compare_keys(const void *a, const void *b) {
  const sort_key *x = a, *y = b;
  for (unsigned i = 0; i < 2; ++i) {
    if (x->missing[i] != y->missing[i])
      return x->missing[i] ? 1 : -1;
    if (x->value[i] != y->value[i])
      return x->value[i] > y->value[i] ? 1 : -1;
  }
  return (x->id > y->id) - (x->id < y->id);
}
static int compare_ids(const void *a, const void *b) {
  const sort_key *x = a, *y = b;
  return (x->id > y->id) - (x->id < y->id);
}
df_status df_view_entities(df_view *v, uint32_t order, uint32_t secondary, df_entities *out) {
  out->count = 0;
  if (!v || v->scope->poisoned || (order && !df_attribute_name(order)) ||
      (secondary && !df_attribute_name(secondary)))
    return DF_ERROR;
  if (!ct_array_reserve(&out->scratch, &out->scratch_capacity, v->facts.size, sizeof(sort_key)))
    return DF_ERROR;
  sort_key *keys = out->scratch;
  for (size_t i = 0; i < v->facts.size; ++i)
    keys[i] = (sort_key){(uint32_t)(v->facts.items[i].key >> 32), {false, false}, {0, 0}};
  if (v->facts.size > 1)
    qsort(keys, v->facts.size, sizeof(*keys), compare_ids);
  size_t count = 0;
  for (size_t i = 0; i < v->facts.size; ++i)
    if (!count || keys[i].id != keys[count - 1].id)
      keys[count++] = keys[i];
  uint32_t attributes[] = {order, secondary};
  for (size_t i = 0; i < count; ++i) {
    for (unsigned j = 0; j < 2; ++j) {
      if (!attributes[j])
        continue;
      df_box *box = df_view_box(v, df_pair_key(keys[i].id, attributes[j]));
      if (!box) {
        keys[i].missing[j] = true;
        continue;
      }
      if (box->size != 9 || box->data[0] != 3)
        return DF_ERROR;
      uint64_t bits = df_integer(df_span(box->data + 1, 8));
      memcpy(&keys[i].value[j], &bits, 8);
    }
  }
  if ((order || secondary) && count > 1)
    qsort(keys, count, sizeof(*keys), compare_keys);
  if (!ct_array_reserve(&out->data, &out->capacity, count, sizeof(*out->data)))
    return DF_ERROR;
  for (size_t i = 0; i < count; ++i)
    out->data[i] = keys[i].id;
  out->count = count;
  return DF_OK;
}
void df_entities_free(df_entities *e) {
  free(e->data);
  free(e->scratch);
  memset(e, 0, sizeof(*e));
}
int df_result_entities(void *p, uint32_t order, uint32_t secondary, df_buffer *out) {
  if (!valid(p, out))
    return 1;
  df_entities e = {0};
  if (df_view_entities(p, order, secondary, &e) != DF_OK) {
    df_entities_free(&e);
    df_error(out, "invalid ordering or out of memory");
    return 1;
  }
  out->data = (uint8_t *)e.data;
  out->size = e.count * sizeof(uint32_t);
  e.data = NULL;
  df_entities_free(&e);
  return 0;
}
