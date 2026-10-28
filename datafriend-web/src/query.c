#include "store.h"
df_view *df_view_new(df_scope *scope) {
  df_view *v = calloc(1, sizeof(*v));
  if (v) {
    v->refs = 1;
    v->scope = scope;
    ++scope->refs;
    v->reset = true;
  }
  return v;
}
void df_view_retain(df_view *v) {
  if (v)
    ++v->refs;
}
void df_view_release(df_view *v) {
  if (!v || --v->refs)
    return;
  df_boxes_free(&v->facts);
  ct_map_free(&v->entities);
  ct_map_free(&v->attributes);
  df_scope_release(v->scope);
  free(v);
}
bool df_view_encode(df_store *s, df_view *v, df_writer *w) {
  df_u32(w, (uint32_t)v->facts.size);
  for (size_t i = 0; i < v->facts.size; ++i) {
    ct_pair p = v->facts.items[i];
    df_u64(w, p.key >> 32);
    df_blob(w, df_text(df_attribute_name((uint32_t)p.key)));
    df_blob(w, df_box_bytes((df_box *)p.value));
  }
  return df_memory(s, !w->failed);
}
df_values *df_values_find(df_column *c, df_bytes bytes, uint64_t hash) {
  ct_pair *p = ct_map_get(&c->values, hash);
  for (df_values *v = p ? (df_values *)p->value : NULL; v; v = v->next)
    if (df_equal(df_box_bytes(v->value), bytes))
      return v;
  return NULL;
}
void df_indexes_free(df_store *s) {
  for (size_t a = 0; a < DF_ATTRIBUTE_COUNT; ++a) {
    df_column *c = &s->columns[a];
    for (size_t i = 0; i < c->values.size; ++i) {
      df_values *v = (df_values *)c->values.items[i].value;
      while (v) {
        df_values *next = v->next;
        df_release(v->value);
        ct_map_free(&v->ids);
        free(v);
        v = next;
      }
    }
    ct_map_free(&c->values);
    df_boxes_free(&c->entities);
  }
}
bool df_index(df_store *s, uint64_t key, df_box *next) {
  uint32_t a = (uint32_t)key, id = (uint32_t)(key >> 32);
  df_column *c = &s->columns[df_attribute_index(a)];
  ct_pair *p = ct_map_get(&c->entities, id);
  if (p) {
    df_box *old = (df_box *)p->value;
    ct_pair *head = ct_map_get(&c->values, old->hash);
    df_values *previous = NULL, *v = (df_values *)head->value;
    while (!df_box_equal(v->value, old)) {
      previous = v;
      v = v->next;
    }
    ct_map_erase(&v->ids, id);
    if (!v->ids.size) {
      if (previous)
        previous->next = v->next;
      else
        head->value = (uintptr_t)v->next;
      df_release(v->value);
      ct_map_free(&v->ids);
      free(v);
      if (!head->value)
        ct_map_erase(&c->values, old->hash);
    }
    ct_map_erase(&c->entities, id);
    df_release(old);
  }
  if (!next)
    return true;
  df_values *v = df_values_find(c, df_box_bytes(next), next->hash);
  if (!v) {
    v = calloc(1, sizeof(*v));
    if (!v)
      return df_fail(s, "out of memory");
    v->value = df_retain(next);
    ct_pair *head = ct_map_get(&c->values, next->hash);
    v->next = head ? (df_values *)head->value : NULL;
    if (!ct_map_put(&c->values, next->hash, (uintptr_t)v)) {
      df_release(v->value);
      free(v);
      return df_fail(s, "out of memory");
    }
  }
  if (!df_memory(s, ct_map_put(&v->ids, id, 0)))
    return false;
  if (!df_memory(s, ct_map_put(&c->entities, id, (uintptr_t)next)))
    return false;
  df_retain(next);
  return true;
}
static bool term_equal(df_term a, df_term b) {
  return a.variable == b.variable && (a.variable >= 0 || df_equal(a.constant, b.constant));
}
static df_term read_term(df_reader *r) {
  df_term t = {-1, {0}};
  unsigned tag = df_read8(r);
  if (!tag)
    t.constant = df_read_blob(r);
  else if (tag == 1) {
    t.variable = df_read8(r);
    if (t.variable >= 64)
      r->failed = true;
  } else
    r->failed = true;
  return t;
}
static bool reference(df_bytes b, uint32_t *id) {
  if (b.size != 9 || b.data[0] != 6)
    return false;
  uint64_t n = df_integer(df_span(b.data + 1, 8));
  if (n > UINT32_MAX)
    return false;
  *id = (uint32_t)n;
  return true;
}
static unsigned priority(df_clause *c) {
  return c->value.variable < 0 ? 0 : c->bind_value ? 1 : !c->projected ? 2 : 3;
}
static bool plan(df_store *s, df_query *q) {
  uint32_t n = q->count;
  unsigned uses[64] = {0};
  q->predicate = -1;
  q->entities = calloc(n ? n : 1, sizeof(*q->entities));
  if (!q->entities)
    return df_fail(s, "out of memory");
  for (uint32_t i = 0; i < n; ++i) {
    df_clause *c = q->clauses + i;
    if (c->entity.variable < 0) {
      uint32_t id;
      if (!reference(c->entity.constant, &id))
        return df_fail(s, "entity constant must be a uint32 reference");
    } else
      ++uses[c->entity.variable];
    if (c->value.variable >= 0)
      ++uses[c->value.variable];
    uint32_t g = 0;
    while (g < q->entity_count && !term_equal(q->entities[g].entity, c->entity))
      ++g;
    if (g == q->entity_count) {
      q->entities[g].entity = c->entity;
      ++q->entity_count;
    }
    q->entities[g].checks[q->entities[g].count++] = (uint8_t)i;
  }
  for (uint32_t i = 0; i < n; ++i) {
    df_clause *c = q->clauses + i;
    c->bind_value = c->value.variable >= 0 && uses[c->value.variable] > 1;
  }
  for (uint32_t i = 0; i < q->entity_count; ++i) {
    df_entity_group *g = q->entities + i;
    for (unsigned j = 1; j < g->count; ++j) {
      uint8_t at = g->checks[j];
      unsigned k = j;
      while (k && priority(q->clauses + at) < priority(q->clauses + g->checks[k - 1])) {
        g->checks[k] = g->checks[k - 1];
        --k;
      }
      g->checks[k] = at;
    }
  }
  q->local = q->entity_count == 1;
  if (q->local && q->entities[0].entity.variable >= 0)
    for (uint32_t i = 0; i < n; ++i)
      if (q->clauses[i].value.variable < 0) {
        q->predicate = (int)i;
        break;
      }
  return true;
}
static bool compile(df_store *s, df_query *q) {
  df_reader r = {df_written(&q->key), false};
  unsigned all = df_read8(&r);
  q->all = all != 0;
  uint32_t n = df_count(&r, 7);
  if (all > 1 || n > 32)
    return df_fail(s, "invalid query");
  q->count = n;
  q->clauses = calloc(n ? n : 1, sizeof(*q->clauses));
  if (!q->clauses)
    return df_fail(s, "out of memory");
  for (uint32_t i = 0; i < n; ++i) {
    df_clause *c = q->clauses + i;
    c->entity = read_term(&r);
    df_bytes a = df_read_blob(&r);
    c->attribute = df_attribute_id(a.data, a.size);
    c->value = read_term(&r);
    if (r.failed || !c->attribute)
      return df_fail(s, "invalid query term or attribute");
  }
  uint32_t projections = df_count(&r, 4);
  if ((all && (n || projections)) || (!all && (!n || !projections)))
    return df_fail(s, "invalid query projection");
  for (uint32_t i = 0; i < projections; ++i) {
    uint32_t at = df_read32(&r);
    if (at >= n)
      return df_fail(s, "invalid projection index");
    q->clauses[at].projected = true;
    q->projected |= UINT32_C(1) << at;
  }
  if (!df_end(&r))
    return df_fail(s, "invalid query packet");
  return plan(s, q);
}
/* Registration keys only choose buckets. Literal equality below handles hash
 * collisions; query identity likewise compares its complete serialized bytes. */
static uint64_t predicate_key(uint32_t a, uint32_t p, uint64_t h) {
  return ct_mix(df_pair_key(a, p)) ^ h;
}
static bool register_route(df_store *s, df_query *q, df_clause *c) {
  ct_map *map;
  uint64_t key;
  uint32_t predicate = 0;
  df_bytes value = {0};
  uint32_t id;
  if (q->predicate >= 0) {
    df_clause *p = q->clauses + q->predicate;
    predicate = p->attribute;
    value = p->value.constant;
    map = &s->predicate_routes;
    key = predicate_key(c->attribute, predicate, df_hash(value));
  } else if (c->entity.variable < 0) {
    if (!reference(c->entity.constant, &id))
      return df_fail(s, "invalid compiled entity reference");
    map = &s->fixed_routes;
    key = df_pair_key(id, c->attribute);
  } else {
    map = &s->general_routes;
    key = c->attribute;
  }
  ct_pair *head = ct_map_get(map, key);
  df_route *route = head ? (df_route *)head->value : NULL;
  while (route && (route->attribute != c->attribute || route->predicate != predicate ||
                   (predicate && !df_equal(df_box_bytes(route->value), value))))
    route = route->next;
  if (!route) {
    route = calloc(1, sizeof(*route));
    if (!route)
      return df_fail(s, "out of memory");
    route->key = key;
    route->attribute = c->attribute;
    route->predicate = predicate;
    if (predicate) {
      route->value = df_box_new(value);
      if (!route->value) {
        free(route);
        return df_fail(s, "out of memory");
      }
    }
    route->next = head ? (df_route *)head->value : NULL;
    if (!ct_map_put(map, key, (uintptr_t)route)) {
      df_release(route->value);
      free(route);
      return df_fail(s, "out of memory");
    }
    if (predicate) {
      ct_pair *p =
          ct_map_get(&s->predicate_attributes[df_attribute_index(c->attribute)], predicate);
      uintptr_t count = p ? p->value : 0;
      if (!df_memory(s, ct_map_put(&s->predicate_attributes[df_attribute_index(c->attribute)],
                                   predicate, count + 1))) {
        if (route->next)
          ct_map_get(map, key)->value = (uintptr_t)route->next;
        else
          ct_map_erase(map, key);
        df_release(route->value);
        free(route);
        return false;
      }
    }
  }
  for (uint32_t i = 0; i < q->route_count; ++i)
    if (q->routes[i] == route)
      return true;
  if (!df_memory(s, ct_map_put(&route->groups, (uintptr_t)q, 0)))
    return false;
  q->routes[q->route_count++] = route;
  return true;
}
static void query_free(df_query *q) {
  df_writer_free(&q->key);
  free(q->clauses);
  free(q->entities);
  df_view_release(q->result);
  ct_map_free(&q->affected);
  free(q);
}
static void unregister_query(df_store *s, df_query *q) {
  ct_map_erase(&s->wildcard, (uintptr_t)q);
  ct_map_erase(&s->affected, (uintptr_t)q);
  for (uint32_t i = 0; i < q->route_count; ++i) {
    df_route *r = q->routes[i];
    ct_map_erase(&r->groups, (uintptr_t)q);
    if (r->groups.size)
      continue;
    ct_map *map = r->predicate ? &s->predicate_routes : NULL;
    if (!map) { /* General keys fit attr; fixed entity zero deliberately uses a separate map. */
      map = &s->general_routes;
      ct_pair *p = ct_map_get(map, r->key);
      if (!p || (df_route *)p->value != r)
        map = &s->fixed_routes;
    }
    ct_pair *head = ct_map_get(map, r->key);
    df_route *prev = NULL, *at = (df_route *)head->value;
    while (at != r) {
      prev = at;
      at = at->next;
    }
    if (prev)
      prev->next = r->next;
    else
      head->value = (uintptr_t)r->next;
    if (!head->value)
      ct_map_erase(map, r->key);
    if (r->predicate) {
      ct_pair *p =
          ct_map_get(&s->predicate_attributes[df_attribute_index(r->attribute)], r->predicate);
      if (p && !--p->value)
        ct_map_erase(&s->predicate_attributes[df_attribute_index(r->attribute)], r->predicate);
    }
    df_release(r->value);
    ct_map_free(&r->groups);
    free(r);
  }
}
static bool subscribe(df_store *s, df_bytes packet, const df_clause *clauses, uint32_t count,
                      bool all, uint64_t *id) {
  if (!df_healthy(s))
    return false;
  if (!s->next_view)
    return df_fail(s, "view IDs exhausted");
  uint64_t hash = df_hash(packet);
  ct_pair *head = ct_map_get(&s->groups, hash);
  df_query *q = head ? (df_query *)head->value : NULL;
  for (; q; q = q->next)
    if (df_equal(df_written(&q->key), packet)) {
      if (!df_memory(s, ct_map_put(&s->views, s->next_view, (uintptr_t)q)))
        return false;
      ++q->owners;
      *id = s->next_view++;
      return true;
    }
  q = calloc(1, sizeof(*q));
  if (!q)
    return df_fail(s, "out of memory");
  df_append(&q->key, packet.data, packet.size);
  if (!df_memory(s, !q->key.failed))
    goto fail;
  if (clauses) {
    q->count = count;
    q->all = all;
    q->clauses = calloc(count ? count : 1, sizeof(*q->clauses));
    if (!df_memory(s, q->clauses != NULL))
      goto fail;
    for (uint32_t i = 0; i < count; ++i) {
      q->clauses[i] = clauses[i];
      df_term *terms[] = {&q->clauses[i].entity, &q->clauses[i].value};
      for (unsigned j = 0; j < 2; ++j)
        if (terms[j]->variable < 0)
          terms[j]->constant.data = q->key.data + (terms[j]->constant.data - packet.data);
      if (q->clauses[i].projected)
        q->projected |= UINT32_C(1) << i;
    }
    if (!plan(s, q))
      goto fail;
  } else if (!compile(s, q))
    goto fail;
  q->result = df_view_new(s->scope);
  if (!df_memory(s, q->result != NULL) || !df_evaluate(s, q, false, 0, false))
    goto fail;
  if (q->all && !df_memory(s, ct_map_put(&s->wildcard, (uintptr_t)q, 0)))
    goto fail;
  for (uint32_t i = 0; i < q->count; ++i)
    if (!register_route(s, q, q->clauses + i))
      goto fail;
  if (!df_memory(s, ct_map_reserve(&s->views, s->views.size + 1)))
    goto fail;
  head = ct_map_get(&s->groups, hash);
  q->next = head ? (df_query *)head->value : NULL;
  if (!df_memory(s, ct_map_put(&s->groups, hash, (uintptr_t)q)))
    goto fail;
  ct_map_put(&s->views, s->next_view, (uintptr_t)q);
  q->owners = 1;
  *id = s->next_view++;
  return true;
fail:
  unregister_query(s, q);
  query_free(q);
  return false;
}
bool df_subscribe(df_store *s, df_bytes packet, uint64_t *id) {
  return subscribe(s, packet, NULL, 0, false, id);
}
bool df_subscribe_direct(df_store *s, df_query_spec spec, uint64_t *id) {
  if (spec.count > 32 || (spec.count && !spec.clauses) ||
      (spec.all ? spec.count != 0 : spec.count == 0))
    return df_fail(s, "invalid query");
  df_clause clauses[32] = {0};
  size_t offsets[32][2] = {{0}};
  df_writer key = {0};
  /* A normalized numeric key deduplicates native queries; constants are stored
   * here once and referenced by compiled clauses. No request decoder involved. */
  df_u8(&key, 2);
  df_u8(&key, spec.all);
  df_u32(&key, (uint32_t)spec.count);
  bool ok = false, projected = spec.all;
  for (size_t i = 0; i < spec.count; ++i) {
    df_match m = spec.clauses[i];
    if (!df_attribute_name(m.attribute)) {
      df_fail(s, "unknown shared attribute");
      goto done;
    }
    clauses[i].attribute = m.attribute;
    clauses[i].projected = m.project;
    projected |= m.project;
    df_u32(&key, m.attribute);
    df_u8(&key, m.project);
    df_match_term input[] = {m.entity, m.value};
    df_term *terms[] = {&clauses[i].entity, &clauses[i].value};
    for (unsigned j = 0; j < 2; ++j) {
      int variable = input[j].variable;
      if (variable < -1 || variable >= 64) {
        df_fail(s, "invalid variable");
        goto done;
      }
      terms[j]->variable = variable;
      df_u8(&key, (uint8_t)(variable + 1));
      if (variable < 0) {
        size_t length_at = key.size;
        df_u32(&key, 0);
        offsets[i][j] = key.size;
        if (!df_encode_value(&key, input[j].value)) {
          df_fail(s, "invalid value or out of memory");
          goto done;
        }
        size_t size = key.size - offsets[i][j];
        terms[j]->constant.size = size;
        for (unsigned k = 0; k < 4; ++k)
          key.data[length_at + k] = (uint8_t)(size >> (24 - 8 * k));
      }
    }
  }
  if (!projected) {
    df_fail(s, "query needs a projection");
    goto done;
  }
  if (!df_memory(s, !key.failed))
    goto done;
  for (size_t i = 0; i < spec.count; ++i) {
    clauses[i].entity.constant.data = key.data + offsets[i][0];
    clauses[i].value.constant.data = key.data + offsets[i][1];
  }
  ok = subscribe(s, df_written(&key), clauses, (uint32_t)spec.count, spec.all, id);
done:
  df_writer_free(&key);
  return ok;
}
void df_unsubscribe(df_store *s, uint64_t id) {
  ct_pair *p = ct_map_get(&s->views, id);
  if (!p)
    return;
  df_query *q = (df_query *)p->value;
  ct_map_erase(&s->views, id);
  if (--q->owners)
    return;
  unregister_query(s, q);
  uint64_t hash = df_hash(df_written(&q->key));
  ct_pair *head = ct_map_get(&s->groups, hash);
  df_query *prev = NULL, *at = (df_query *)head->value;
  while (at != q) {
    prev = at;
    at = at->next;
  }
  if (prev)
    prev->next = q->next;
  else
    head->value = (uintptr_t)q->next;
  if (!head->value)
    ct_map_erase(&s->groups, hash);
  query_free(q);
}
void df_queries_free(df_store *s) {
  while (s->views.size)
    df_unsubscribe(s, s->views.items[s->views.size - 1].key);
  ct_map_free(&s->views);
  ct_map_free(&s->groups);
  ct_map *maps[] = {&s->general_routes, &s->fixed_routes, &s->predicate_routes};
  for (unsigned j = 0; j < 3; ++j) {
    ct_map *m = maps[j];
    for (size_t i = 0; i < m->size; ++i) {
      df_route *r = (df_route *)m->items[i].value;
      while (r) {
        df_route *next = r->next;
        ct_map_free(&r->groups);
        df_release(r->value);
        free(r);
        r = next;
      }
    }
    ct_map_free(m);
  }
  for (unsigned a = 0; a < DF_ATTRIBUTE_COUNT; ++a)
    ct_map_free(&s->predicate_attributes[a]);
  ct_map_free(&s->wildcard);
  ct_map_free(&s->affected);
}
static bool visit(df_store *s, ct_map *groups, uint32_t entity) {
  for (size_t i = 0; i < groups->size; ++i) {
    df_query *q = (df_query *)(uintptr_t)groups->items[i].key;
    if (!q->scheduled) {
      if (!df_memory(s, ct_map_put(&s->affected, (uintptr_t)q, 0)))
        return false;
      q->scheduled = true;
    }
    if (q->local && !df_memory(s, ct_map_put(&q->affected, entity, 0)))
      return false;
  }
  return true;
}
void df_routes_reset(df_store *s) {
  for (size_t i = 0; i < s->affected.size; ++i) {
    df_query *q = (df_query *)(uintptr_t)s->affected.items[i].key;
    q->scheduled = q->updated = false;
    ct_map_clear(&q->affected);
  }
  ct_map_clear(&s->affected);
}
bool df_route_changes(df_store *s, const ct_map *changes, bool predicates_only) {
  if (!predicates_only && !visit(s, &s->wildcard, 0))
    return false;
  for (size_t i = 0; i < changes->size; ++i) {
    uint64_t key = changes->items[i].key;
    uint32_t a = (uint32_t)key, e = (uint32_t)(key >> 32);
    ct_pair *p;
    if (!predicates_only) {
      p = ct_map_get(&s->general_routes, a);
      if (p && !visit(s, &((df_route *)p->value)->groups, e))
        return false;
      p = ct_map_get(&s->fixed_routes, key);
      if (p && !visit(s, &((df_route *)p->value)->groups, e))
        return false;
    }
    ct_map *predicates = &s->predicate_attributes[df_attribute_index(a)];
    for (size_t j = 0; j < predicates->size; ++j) {
      uint32_t pred = (uint32_t)predicates->items[j].key;
      df_box *value = df_current(s, df_pair_key(e, pred));
      if (!value)
        continue;
      p = ct_map_get(&s->predicate_routes, predicate_key(a, pred, value->hash));
      for (df_route *r = p ? (df_route *)p->value : NULL; r; r = r->next)
        if (r->attribute == a && r->predicate == pred && df_box_equal(r->value, value) &&
            !visit(s, &r->groups, e))
          return false;
    }
  }
  return true;
}
typedef struct evaluator {
  df_store *store;
  df_query *query;
  df_view *out;
  df_bytes bound[64];
  bool bound_set[64];
  uint64_t matched_keys[32];
  df_box *matched_values[32];
  ct_map seen;
  bool local, track, changed;
  uint32_t entity;
} evaluator;
static bool resolve(evaluator *e, df_term t, df_bytes *out) {
  if (t.variable < 0) {
    *out = t.constant;
    return true;
  }
  if (!e->bound_set[t.variable])
    return false;
  *out = e->bound[t.variable];
  return true;
}
static bool mark(evaluator *e, uint64_t key) {
  return !e->track || (df_memory(e->store, ct_map_put(&e->out->entities, key >> 32, 0)) &&
                       df_memory(e->store, ct_map_put(&e->out->attributes, (uint32_t)key, 0)));
}
static bool apply(evaluator *e, uint64_t key, df_box *value) {
  if (!df_memory(e->store, ct_map_put(&e->seen, key, 0)))
    return false;
  ct_pair *old = ct_map_get(&e->out->facts, key);
  if (old && df_box_equal((df_box *)old->value, value))
    return true;
  if (!mark(e, key))
    return false;
  if (old) {
    df_release((df_box *)old->value);
    old->value = (uintptr_t)df_retain(value);
  } else {
    if (!df_memory(e->store, ct_map_put(&e->out->facts, key, (uintptr_t)value)))
      return false;
    df_retain(value);
  }
  e->changed = true;
  return true;
}
typedef struct access_path {
  const ct_map *ids;
  size_t count;
  bool point;
  uint32_t entity;
} access_path;
static access_path choose(evaluator *e, df_entity_group *g) {
  access_path path = {NULL, SIZE_MAX, false, 0};
  df_bytes b;
  if (resolve(e, g->entity, &b)) {
    path.point = true;
    if (!reference(b, &path.entity))
      path.count = 0;
    else
      path.count = 1;
    return path;
  }
  if (e->local) {
    path.point = true;
    path.entity = e->entity;
    path.count = 1;
    return path;
  }
  for (unsigned j = 0; j < g->count; ++j) {
    df_clause *c = e->query->clauses + g->checks[j];
    df_column *column = &e->store->columns[df_attribute_index(c->attribute)];
    const ct_map *ids = &column->entities;
    if (resolve(e, c->value, &b)) {
      df_values *v = df_values_find(column, b, df_hash(b));
      if (!v) {
        path.count = 0;
        return path;
      }
      ids = &v->ids;
    }
    if (ids->size < path.count) {
      path.ids = ids;
      path.count = ids->size;
    }
  }
  return path;
}
static bool bind(evaluator *e, df_term term, df_bytes value, uint8_t added[64], unsigned *count) {
  if (term.variable < 0)
    return df_equal(term.constant, value);
  unsigned at = (unsigned)term.variable;
  if (e->bound_set[at])
    return df_equal(e->bound[at], value);
  e->bound_set[at] = true;
  e->bound[at] = value;
  added[(*count)++] = (uint8_t)at;
  return true;
}
static bool walk(evaluator *, uint32_t);
static bool consider(evaluator *e, df_entity_group *g, uint32_t id, uint32_t remaining) {
  uint8_t entity[9] = {6}, added[64];
  unsigned count = 0;
  df_store64(entity + 1, id);
  bool matches = bind(e, g->entity, df_span(entity, 9), added, &count), ok = true;
  for (unsigned j = 0; matches && j < g->count; ++j) {
    unsigned at = g->checks[j];
    df_clause *c = e->query->clauses + at;
    uint64_t key = df_pair_key(id, c->attribute);
    df_box *value = df_current(e->store, key);
    if (!value) {
      matches = false;
      break;
    }
    if (c->value.variable < 0 || c->bind_value)
      matches = bind(e, c->value, df_box_bytes(value), added, &count);
    if (c->projected) {
      e->matched_keys[at] = key;
      e->matched_values[at] = value;
    }
  }
  if (matches)
    ok = walk(e, remaining);
  for (unsigned i = 0; i < count; ++i)
    e->bound_set[added[i]] = false;
  return ok;
}
static bool walk(evaluator *e, uint32_t remaining) {
  if (!remaining) {
    for (uint32_t i = 0; i < e->query->count; ++i)
      if (e->query->clauses[i].projected && !apply(e, e->matched_keys[i], e->matched_values[i]))
        return false;
    return true;
  }
  unsigned selected = 0;
  access_path best = {NULL, SIZE_MAX, false, 0};
  for (unsigned i = 0; i < e->query->entity_count; ++i)
    if (remaining & (UINT32_C(1) << i)) {
      access_path p = choose(e, e->query->entities + i);
      if (!p.count)
        return true;
      if (p.count < best.count) {
        best = p;
        selected = i;
      }
    }
  df_entity_group *g = e->query->entities + selected;
  remaining &= ~(UINT32_C(1) << selected);
  if (best.point)
    return consider(e, g, best.entity, remaining);
  for (size_t i = 0; i < best.ids->size; ++i)
    if (!consider(e, g, (uint32_t)best.ids->items[i].key, remaining))
      return false;
  return true;
}
static bool local_present(df_query *q, uint32_t id) {
  for (uint32_t i = 0; i < q->count; ++i)
    if (q->clauses[i].projected &&
        ct_map_get(&q->result->facts, df_pair_key(id, q->clauses[i].attribute)))
      return true;
  return false;
}
static bool prune(evaluator *e, uint64_t key) {
  if (ct_map_get(&e->seen, key))
    return true;
  ct_pair *p = ct_map_get(&e->out->facts, key);
  if (!p)
    return true;
  if (!mark(e, key))
    return false;
  df_release((df_box *)p->value);
  ct_map_erase(&e->out->facts, key);
  e->changed = true;
  return true;
}
bool df_evaluate(df_store *s, df_query *q, bool local, uint32_t id, bool track) {
  evaluator e = {0};
  e.store = s;
  e.query = q;
  e.out = q->result;
  e.local = local;
  e.entity = id;
  e.track = track;
  bool had = local && local_present(q, id), ok = false;
  ct_map before = {0};
  if (track && !local)
    for (size_t i = 0; i < e.out->facts.size; ++i)
      if (!df_memory(s, ct_map_put(&before, e.out->facts.items[i].key >> 32, 0)))
        goto done;
  if (q->all) {
    for (unsigned a = 0; a < DF_ATTRIBUTE_COUNT; ++a) {
      ct_map *m = &s->columns[a].entities;
      for (size_t i = 0; i < m->size; ++i)
        if (!apply(&e, df_pair_key((uint32_t)m->items[i].key, df_attributes[a].id),
                   (df_box *)m->items[i].value))
          goto done;
    }
  } else {
    bool eligible = true;
    if (local && q->entities[0].entity.variable < 0) {
      uint32_t fixed;
      eligible = reference(q->entities[0].entity.constant, &fixed) && fixed == id;
    }
    if (eligible &&
        !walk(&e, q->entity_count == 32 ? UINT32_MAX : (UINT32_C(1) << q->entity_count) - 1))
      goto done;
  }
  if (local) {
    for (unsigned i = 0; i < q->count; ++i)
      if (q->clauses[i].projected && !prune(&e, df_pair_key(id, q->clauses[i].attribute)))
        goto done;
    if (track)
      e.out->membership |= had != local_present(q, id);
  } else {
    for (size_t i = 0; i < e.out->facts.size;) {
      uint64_t key = e.out->facts.items[i].key;
      if (ct_map_get(&e.seen, key)) {
        ++i;
        continue;
      }
      if (!prune(&e, key))
        goto done;
    }
    if (track && e.changed) {
      ct_map after = {0};
      for (size_t i = 0; i < e.out->facts.size; ++i)
        if (!df_memory(s, ct_map_put(&after, e.out->facts.items[i].key >> 32, 0))) {
          ct_map_free(&after);
          goto done;
        }
      bool same = before.size == after.size;
      for (size_t i = 0; same && i < before.size; ++i)
        same = ct_map_get(&after, before.items[i].key) != NULL;
      e.out->membership |= !same;
      ct_map_free(&after);
    }
  }
  q->updated |= e.changed;
  ok = true;
done:
  ct_map_free(&e.seen);
  ct_map_free(&before);
  return ok;
}
