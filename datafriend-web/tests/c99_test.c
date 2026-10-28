#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "datafriend/core.h"
#include "datafriend/network.h"
#include "common.h"
#include <unistd.h>
#include <assert.h>
static void require(bool ok, const char *message) {
  if (!ok) {
    fprintf(stderr, "%s\n", message);
    abort();
  }
}
static void clean(const char *path) {
  char file[256];
  const char *names[] = {"data.mdb", "lock.mdb", "datafriend.owner"};
  for (unsigned i = 0; i < 3; ++i) {
    snprintf(file, sizeof(file), "%s/%s", path, names[i]);
    unlink(file);
  }
  rmdir(path);
}
static void map_churn(void) {
  ct_map map = {0};
  uintptr_t model[1024] = {0};
  uint64_t random = 1;
  size_t count = 0;
  for (unsigned turn = 0; turn < 200000; ++turn) {
    random = ct_mix(random);
    unsigned id = (unsigned)(random % 1024);
    uint64_t key = id == 0 ? UINT64_MAX : id - 1;
    if (random & 4096) {
      if (!model[id])
        ++count;
      model[id] = (uintptr_t)turn + 1;
      require(ct_map_put(&map, key, model[id]), "map insertion");
    } else {
      bool present = model[id] != 0;
      require(ct_map_erase(&map, key) == present, "map erase");
      if (present)
        --count;
      model[id] = 0;
    }
    require(map.size == count, "dense map count");
    ct_pair *p = ct_map_get(&map, key);
    require(model[id] ? (p && p->value == model[id]) : !p, "map lookup after collision/backshift");
    if (!(turn % 1000))
      for (unsigned i = 0; i < 1024; ++i) {
        p = ct_map_get(&map, i == 0 ? UINT64_MAX : i - 1);
        require(model[i] ? (p && p->value == model[i]) : !p, "map moved entry");
      }
  }
  ct_map_free(&map);
}
static df_writer query(void) {
  df_writer w = {0};
  df_u8(&w, 1);
  df_u8(&w, 1);
  df_u8(&w, 0);
  df_u32(&w, 2);
  df_u8(&w, 1);
  df_u8(&w, 0);
  df_blob(&w, df_text("title"));
  df_u8(&w, 0);
  df_blob(&w, df_text("x"));
  df_u8(&w, 1);
  df_u8(&w, 0);
  df_blob(&w, df_text("color"));
  df_u8(&w, 1);
  df_u8(&w, 1);
  df_u32(&w, 2);
  df_u32(&w, 0);
  df_u32(&w, 1);
  return w;
}
static df_writer edit(void) {
  df_writer w = {0};
  df_u8(&w, 1);
  df_u8(&w, 10);
  df_u32(&w, 0);
  df_u32(&w, 2);
  df_u64(&w, 7);
  df_blob(&w, df_text("title"));
  df_blob(&w, df_text("x"));
  df_u64(&w, 7);
  df_blob(&w, df_text("color"));
  df_blob(&w, df_text("blue"));
  return w;
}
static void allocation_recovery(void) {
  df_writer q = query(), change = edit();
  unsigned failures = 0, successes = 0;
  for (int64_t fail = 0; fail < 128; ++fail) {
    char path[] = "/tmp/datafriend-c99-XXXXXX";
    require(mkdtemp(path) != NULL, "temporary directory");
    df_buffer out = {0};
    void *db = df_open_sized((const uint8_t *)path, strlen(path), (const uint8_t *)"default", 7, 0,
                             1 << 24, &out);
    require(db != NULL, "open C store");
    require(!df_call(db, q.data, q.size, &out), "subscribe C query");
    uint64_t id = df_integer(df_span(out.data, out.size));
    df_free(out.data);
    void *result = df_result(db, id, &out);
    require(result != NULL, "retain C view");
    df_test_allocation_failure(fail);
    int rc = df_call(db, change.data, change.size, &out);
    df_test_allocation_failure(-1);
    df_free(out.data);
    require(df_view_valid(result), "failed allocation poisoned recoverable view");
    if (rc) {
      ++failures;
      require(df_revision(db) == 0 && df_view_size(result) == 0,
              "failed allocation published partial edit");
      require(!df_call(db, change.data, change.size, &out),
              "writer unusable after allocation recovery");
      df_free(out.data);
    } else
      ++successes;
    require(df_revision(db) == 1 && df_view_size(result) == 2, "successful edit lost facts");
    df_buffer value = {0};
    require(!df_result_value(result, 7, 2, &value) && value.size == 1 && value.data[0] == 'x',
            "direct C borrowed read");
    df_close(db);
    require(df_view_size(result) == 2, "C result lost at store close");
    df_result_release(result);
    clean(path);
  }
  require(failures > 20 && successes > 0, "allocation failure coverage");
  char path[] = "/tmp/datafriend-c99-XXXXXX";
  require(mkdtemp(path) != NULL, "temporary directory");
  df_buffer out = {0};
  void *db = df_open((const uint8_t *)path, strlen(path), 0, &out);
  require(db != NULL, "open");
  for (int64_t fail = 0; fail < 80; ++fail) {
    df_test_allocation_failure(fail);
    int rc = df_call(db, q.data, q.size, &out);
    df_test_allocation_failure(-1);
    if (!rc) {
      uint64_t id = df_integer(df_span(out.data, out.size));
      df_writer remove = {0};
      df_u8(&remove, 1);
      df_u8(&remove, 4);
      df_u64(&remove, id);
      df_free(out.data);
      require(!df_call(db, remove.data, remove.size, &out),
              "unsubscribe after failed registration");
      df_writer_free(&remove);
    }
    df_free(out.data);
  }
  /* A malformed variable must be rejected before indexing the 64 binding slots. */
  q.data[8] = 255;
  require(df_call(db, q.data, q.size, &out) != 0, "out of range wire variable accepted");
  df_free(out.data);
  df_close(db);
  clean(path);
  df_writer_free(&q);
  df_writer_free(&change);
}
int main(void) {
  map_churn();
  allocation_recovery();
  puts("PASS C99: dense-map churn, allocation rollback, retained views, malformed query rejection");
  return 0;
}
