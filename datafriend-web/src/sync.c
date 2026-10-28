#include "store.h"
#include "datafriend/core.h"
#include "../protocol/reconcile.h"
struct df_snapshot {
  MDB_txn *tx;
  MDB_dbi db;
  uint64_t revision;
  ct_map have, need, incoming;
  ct_pair *outgoing, *fetching;
  size_t outgoing_count, fetching_count, push_offset, fetch_offset;
  bool complete, prepared;
};
static void snapshot_free(df_snapshot *s) {
  if (!s)
    return;
  if (s->tx)
    mdb_txn_abort(s->tx);
  ct_map_free(&s->have);
  ct_map_free(&s->need);
  df_boxes_free(&s->incoming);
  free(s->outgoing);
  free(s->fetching);
  free(s);
}
void df_sync_cancel(df_store *s) {
  snapshot_free(s->snapshot);
  s->snapshot = NULL;
}
static df_snapshot *snapshot_new(df_store *store) {
  df_snapshot *s = calloc(1, sizeof(*s));
  if (!df_memory(store, s != NULL))
    return NULL;
  s->db = store->db;
  s->revision = store->revision;
  if (!df_check(store, mdb_txn_begin(store->env, NULL, MDB_RDONLY, &s->tx))) {
    free(s);
    return NULL;
  }
  return s;
}
static bool summary(void *ctx, lb_recon_range part, lb_recon_summary *out) {
  df_snapshot *s = ctx;
  uint8_t lo[8], hi[8];
  df_store64(lo, part.low);
  df_store64(hi, part.high);
  MDB_val low = {8, lo}, high = {8, hi};
  MDB_agg a = {0};
  if (mdb_agg_range(s->tx, s->db, &low, NULL, part.high ? &high : NULL, NULL, MDB_RANGE_LOWER_INCL,
                    &a))
    return false;
  out->count = a.mv_agg_entries;
  out->fingerprint = df_integer(df_span(a.mv_agg_hashes, 8));
  return true;
}
static bool select_id(void *ctx, uint64_t rank, uint64_t *out) {
  df_snapshot *s = ctx;
  MDB_val k = {0}, v = {0};
  if (mdb_agg_select(s->tx, s->db, MDB_AGG_WEIGHT_ENTRIES, rank, &k, &v, NULL) || k.mv_size != 8)
    return false;
  *out = df_integer(df_span(k.mv_data, 8));
  return true;
}
static size_t list(void *ctx, lb_recon_range part, lb_recon_item *out, size_t capacity) {
  df_snapshot *s = ctx;
  MDB_cursor *c = NULL;
  if (mdb_cursor_open(s->tx, s->db, &c))
    return SIZE_MAX;
  uint8_t key[8];
  df_store64(key, part.low);
  MDB_val k = {8, key}, v = {0};
  int rc = mdb_cursor_get(c, &k, &v, MDB_SET_RANGE);
  size_t n = 0;
  while (!rc && n < capacity) {
    if (k.mv_size != 8 || v.mv_size < 8) {
      n = SIZE_MAX;
      break;
    }
    uint64_t id = df_integer(df_span(k.mv_data, 8));
    if (part.high && id >= part.high)
      break;
    out[n++] = (lb_recon_item){id, df_integer(df_span(v.mv_data, 8))};
    rc = mdb_cursor_get(c, &k, &v, MDB_NEXT);
  }
  mdb_cursor_close(c);
  return rc && rc != MDB_NOTFOUND ? SIZE_MAX : n;
}
static lb_recon_source source(df_snapshot *s) {
  lb_recon_source src = {s, summary, select_id, list};
  return src;
}
static bool record(df_store *store, df_snapshot *s, uint64_t id, df_bytes *out) {
  uint8_t bytes[8];
  df_store64(bytes, id);
  MDB_val k = {8, bytes}, v = {0};
  if (!df_check(store, mdb_get(s->tx, s->db, &k, &v)))
    return false;
  *out = df_span(v.mv_data, v.mv_size);
  return true;
}
static bool begin(df_store *store, df_snapshot *s, df_writer *reply) {
  lb_recon_workspace work = {0};
  uint8_t buffer[4096];
  lb_recon_output out = {buffer, sizeof(buffer), 0, 0};
  if (lb_recon_begin(source(s), &work, &out) != lb_recon_status_complete)
    return df_fail(store, "reconciliation source failure");
  df_append(reply, buffer, out.size);
  return df_memory(store, !reply->failed);
}
static bool receive(df_store *store, df_snapshot *s, df_bytes packet, df_writer *reply) {
  lb_recon_workspace work = {0};
  lb_recon_cursor cursor = {0};
  uint8_t buffer[4096];
  uint64_t have[32], need[32];
  lb_recon_differences diff = {have, 32, 0, need, 32, 0};
  lb_recon_output out = {buffer, sizeof(buffer), 0, 0};
  df_append(reply, "LBR\3", 4);
  for (size_t turns = 0;; ++turns) {
    if (turns > 1000000)
      return df_fail(store, "reconciliation work limit");
    lb_recon_bytes input = {packet.data, packet.size};
    lb_recon_status status = lb_recon_receive(source(s), lb_recon_role_initiator, input, &cursor,
                                              &work, &out, &diff, 64);
    if (status != lb_recon_status_complete && status != lb_recon_status_more &&
        status != lb_recon_status_output_full && status != lb_recon_status_differences_full)
      return df_fail(store, "invalid reconciliation packet or source");
    if (out.size > 4)
      df_append(reply, buffer + 4, out.size - 4);
    for (size_t i = 0; i < diff.have_count; ++i)
      if (!df_memory(store, ct_map_put(&s->have, have[i], 0)))
        return false;
    for (size_t i = 0; i < diff.need_count; ++i)
      if (!df_memory(store, ct_map_put(&s->need, need[i], 0)))
        return false;
    if (!df_memory(store, !reply->failed))
      return false;
    if (reply->size > 8 * 1024 * 1024 - 2048 || s->have.size > 1000000 || s->need.size > 1000000)
      return df_fail(store, "reconciliation result limit");
    if (status == lb_recon_status_complete)
      return true;
  }
}
static bool prepare(df_store *store, df_snapshot *s) {
  ct_map ids = {0};
  bool ok = false;
  for (size_t i = 0; i < s->have.size; ++i)
    if (!df_memory(store, ct_map_put(&ids, s->have.items[i].key, 0)))
      goto done;
  for (size_t i = 0; i < s->need.size; ++i)
    if (!df_memory(store, ct_map_put(&ids, s->need.items[i].key, 0)))
      goto done;
  ct_pair *fetching = malloc((s->need.size ? s->need.size : 1) * sizeof(*fetching));
  if (!df_memory(store, fetching != NULL))
    goto done;
  if (s->need.size)
    memcpy(fetching, s->need.items, s->need.size * sizeof(*fetching));
  qsort(fetching, s->need.size, sizeof(*fetching), ct_key_compare);
  if (ids.size > 1)
    qsort(ids.items, ids.size, sizeof(*ids.items), ct_key_compare);
  s->outgoing = ids.items;
  s->outgoing_count = ids.size;
  ids.items = NULL;
  s->fetching = fetching;
  s->fetching_count = s->need.size;
  s->prepared = true;
  ok = true;
done:
  ct_map_free(&ids);
  return ok;
}
bool df_sync(df_store *store, unsigned op, df_bytes packet, df_writer *w) {
  if (op == 4) {
    df_sync_cancel(store);
    return true;
  }
  if (!df_healthy(store))
    return false;
  if (op == 0 || op == 5) {
    df_sync_cancel(store);
    store->snapshot = snapshot_new(store);
    if (!store->snapshot)
      return false;
    if (op == 0)
      return begin(store, store->snapshot, w);
    op = 1;
  }
  df_snapshot *s = store->snapshot;
  if (!s)
    return df_fail(store, "no active sync");
  if (op == 1) {
    if (s->complete)
      return df_fail(store, "sync already complete");
    if (!receive(store, s, packet, w))
      return false;
    s->complete = w->size == 4;
    return true;
  }
  if (!s->complete)
    return df_fail(store, "sync is not complete");
  if (!s->prepared && !prepare(store, s))
    return false;
  if (op == 2) {
    uint32_t count = 0;
    df_u32(w, 0);
    while (s->push_offset < s->outgoing_count && w->size - 4 < 256 * 1024) {
      uint64_t id = s->outgoing[s->push_offset++].key;
      bool present = ct_map_get(&s->have, id) != NULL;
      ++count;
      df_u64(w, id);
      df_u8(w, present);
      if (present) {
        df_bytes b;
        if (!record(store, s, id, &b))
          return false;
        df_blob(w, b);
      }
    }
    if (!df_memory(store, !w->failed))
      return false;
    for (unsigned i = 0; i < 4; ++i)
      w->data[i] = (uint8_t)(count >> (24 - 8 * i));
  } else if (op == 3) {
    size_t count = s->fetching_count - s->fetch_offset;
    if (count > 64)
      count = 64;
    df_u8(w, s->fetch_offset + count == s->fetching_count);
    df_u32(w, (uint32_t)count);
    for (size_t i = 0; i < count; ++i)
      df_u64(w, s->fetching[s->fetch_offset++].key);
  } else
    return df_fail(store, "unknown sync operation");
  if (w->size > 8 * 1024 * 1024 - 2048)
    return df_fail(store, "sync transfer exceeds frame limit");
  return df_memory(store, !w->failed);
}
bool df_sync_apply(df_store *store, df_bytes packet, df_writer *out) {
  df_snapshot *s = store->snapshot;
  if (!s || !s->complete)
    return df_fail(store, "no completed sync");
  if (s->revision != store->revision)
    return df_fail(store, "local data changed during pull; retry");
  df_reader r = {packet, false};
  uint32_t n = df_count(&r, 12);
  ct_map batch = {0};
  bool ok = false;
  for (uint32_t i = 0; i < n; ++i) {
    uint64_t id = df_read64(&r);
    df_bytes record_bytes = df_read_blob(&r), value;
    if (r.failed || !ct_map_get(&s->need, id) || ct_map_get(&s->incoming, id) || ct_map_get(&batch, id)) {
      df_fail(store, "unexpected or duplicate transferred fact");
      goto done;
    }
    if (!df_fact_decode(store, id, record_bytes, &value))
      goto done;
    df_box *box = df_box_new(value);
    if (!df_memory(store, box != NULL))
      goto done;
    if (!ct_map_put(&batch, id, (uintptr_t)box)) {
      df_release(box);
      df_fail(store, "out of memory");
      goto done;
    }
  }
  if (!df_end(&r)) {
    df_fail(store, "invalid transfer packet");
    goto done;
  }
  if (!df_memory(store, ct_map_reserve(&s->incoming, s->incoming.size + batch.size)))
    goto done;
  for (size_t i = 0; i < batch.size; ++i)
    ct_map_put(&s->incoming, batch.items[i].key, batch.items[i].value);
  ct_map_clear(&batch);
  if (s->incoming.size != s->need.size) {
    if (!n) {
      df_fail(store, "incomplete fact transfer");
      goto done;
    }
    df_u32(out, 0);
    ok = df_memory(store, !out->failed);
    goto done;
  }
  for (size_t i = 0; i < s->have.size; ++i) {
    uint64_t id = s->have.items[i].key;
    if (!ct_map_get(&s->need, id) && !df_memory(store, ct_map_put(&s->incoming, id, 0)))
      goto done;
  }
  /* Values moved into incoming above; release the empty staging table before
   * taking ownership of the complete transfer. */
  ct_map_free(&batch);
  batch = s->incoming;
  memset(&s->incoming, 0, sizeof(s->incoming));
  df_sync_cancel(store);
  ok = df_commit(store, &batch, NULL, out, NULL);
done:
  df_boxes_free(&batch);
  return ok;
}
void df_track(void *handle) {
  df_store *s = handle;
  s->tracking = true;
  ct_map_clear(&s->journal);
  s->journal_floor = s->revision;
}
bool df_sync_delta(df_store *store, uint64_t after, df_writer *out, bool *available) {
  *available = false;
  if (!df_healthy(store))
    return false;
  if (!store->tracking || after < store->journal_floor || after > store->revision)
    return true;
  df_snapshot *s = snapshot_new(store);
  if (!s)
    return false;
  bool ok = false;
  uint64_t fingerprint;
  if (!df_fingerprint(store, s->tx, &fingerprint))
    goto done;
  df_u64(out, fingerprint);
  df_u32(out, 0);
  uint32_t count = 0;
  for (size_t i = 0; i < store->journal.size; ++i) {
    ct_pair p = store->journal.items[i];
    if (p.value <= after)
      continue;
    df_box *v = df_current(store, p.key);
    ++count;
    df_u64(out, p.key);
    df_u8(out, v != NULL);
    if (v) {
      df_bytes b;
      if (!record(store, s, p.key, &b))
        goto done;
      df_blob(out, b);
    }
    if (out->size - 12 > 256 * 1024) {
      out->size = 0;
      ok = true;
      goto done;
    }
  }
  if (!df_memory(store, !out->failed))
    goto done;
  for (unsigned i = 0; i < 4; ++i)
    out->data[8 + i] = (uint8_t)(count >> (24 - 8 * i));
  df_sync_cancel(store);
  store->snapshot = s;
  s = NULL;
  *available = true;
  ok = true;
done:
  snapshot_free(s);
  return ok;
}
bool df_sync_apply_delta(df_store *store, df_bytes packet, df_writer *out, bool *mismatch) {
  df_reader r = {packet, false};
  uint64_t expected = df_read64(&r);
  uint32_t n = df_count(&r, 9);
  ct_map changes = {0};
  bool ok = false;
  for (uint32_t i = 0; i < n; ++i) {
    uint64_t id = df_read64(&r);
    unsigned present = df_read8(&r);
    df_bytes value = {0};
    if (present) {
      df_bytes b = df_read_blob(&r);
      if (!df_fact_decode(store, id, b, &value))
        goto done;
    }
    if (r.failed || present > 1 || ct_map_get(&changes, id)) {
      df_fail(store, "invalid or duplicate delta key");
      goto done;
    }
    df_box *box = present ? df_box_new(value) : NULL;
    if (present && !df_memory(store, box != NULL))
      goto done;
    if (!ct_map_put(&changes, id, (uintptr_t)box)) {
      df_release(box);
      df_fail(store, "out of memory");
      goto done;
    }
  }
  if (!df_end(&r)) {
    df_fail(store, "invalid delta packet");
    goto done;
  }
  df_sync_cancel(store);
  ok = df_commit(store, &changes, &expected, out, mismatch);
done:
  df_boxes_free(&changes);
  return ok;
}
int df_delta(void *handle, uint64_t after, df_buffer *out) {
  df_store *s = handle;
  *out = (df_buffer){0};
  df_writer w = {0};
  bool available = false;
  if (!s || !df_sync_delta(s, after, &w, &available)) {
    df_writer_free(&w);
    df_error(out, s ? s->error : "closed engine");
    return 1;
  }
  if (!available) {
    df_writer_free(&w);
    return 2;
  }
  return df_reply(&w, out);
}
int df_apply_delta(void *handle, const uint8_t *bytes, size_t size, df_buffer *out) {
  df_store *s = handle;
  *out = (df_buffer){0};
  df_writer w = {0};
  bool mismatch = false;
  if (!s || (!bytes && size) || !df_sync_apply_delta(s, df_span(bytes, size), &w, &mismatch)) {
    df_writer_free(&w);
    df_error(out, s && s->error ? s->error : "invalid delta");
    if (s) df_notify_reset(s);
    return 1;
  }
  if (mismatch) {
    df_writer_free(&w);
    return 2;
  }
  df_notify(s, df_written(&w));
  return df_reply(&w, out);
}
