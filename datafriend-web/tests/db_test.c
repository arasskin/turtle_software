#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "datafriend/db.h"
#include "datafriend/network.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x)) {                                                                                    \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                      \
      abort();                                                                                     \
    }                                                                                              \
  } while (0)
static void clean(const char *path) {
  char file[256];
  const char *names[] = {"data.mdb", "lock.mdb", "datafriend.owner"};
  for (unsigned i = 0; i < 3; ++i) {
    snprintf(file, sizeof(file), "%s/%s", path, names[i]);
    unlink(file);
  }
  rmdir(path);
}
static int64_t number(df_store *s, uint32_t id, uint32_t a) {
  df_slice b;
  int64_t n;
  CHECK(df_store_get(s, id, a, &b) == DF_OK);
  CHECK(df_value_int(b, &n) == DF_OK);
  return n;
}
typedef struct context {
  df_store *s;
  df_watch *remove, *added;
  unsigned calls;
  bool mutate;
} context;
static void count(void *p, df_view *v) {
  context *c = p;
  CHECK(df_view_valid(v));
  ++c->calls;
  if (!c->mutate)
    return;
  c->mutate = false;
  df_watch_remove(c->s, &c->remove);
  uint32_t a = DF_ATTR_ORDER;
  c->added = df_watch_entity(c->s, 1, &a, 1, NULL, NULL);
  CHECK(c->added);
  df_edit e = {0};
  CHECK(df_edit_set(&e, 1, a, df_int(999)));
  CHECK(df_edit_apply(c->s, &e) == DF_ERROR); /* No recursive same-store edits. */
  df_edit_free(&e);
}
int main(void) {
  char path[] = "/tmp/datafriend-db-XXXXXX";
  CHECK(mkdtemp(path));
  df_options options = {path, "native", DF_BUFFERED, 1 << 24};
  df_buffer error = {0};
  df_store *s = df_store_open(&options, &error);
  if (!s)
    fprintf(stderr, "%.*s\n", (int)error.size, error.data);
  CHECK(s);
  df_free(error.data);
  df_match q[] = {{df_var(0), DF_ATTR_TITLE, df_var(1), true},
                  {df_var(0), DF_ATTR_ORDER, df_var(2), true}};
  df_query_spec spec = {q, 2, false};
  context c = {s, NULL, NULL, 0, false};
  df_watch *w = df_watch_query(s, spec, count, &c);
  CHECK(w && c.calls == 1);
  c.remove = df_watch_query(s, spec, NULL, NULL);
  CHECK(c.remove && c.remove->result == w->result);
  df_edit e = {0};
  uint32_t id = df_edit_create(&e, s);
  CHECK(id == 1);
  char text[] = "first";
  CHECK(df_edit_set(&e, id, DF_ATTR_TITLE, df_string_z(text)));
  text[0] = 'X'; /* Builder owns its input. */
  CHECK(df_edit_set(&e, id, DF_ATTR_ORDER, df_int(7)));
  c.mutate = true;
  CHECK(df_edit_apply(s, &e) == DF_OK && c.calls == 2 && !c.remove && c.added);
  CHECK(number(s, 1, DF_ATTR_ORDER) == 7);
  df_slice b, t;
  CHECK(df_store_get(s, 1, DF_ATTR_TITLE, &b) == DF_OK);
  CHECK(df_value_text(b, &t) == DF_OK && t.size == 5 && !memcmp(t.data, "first", 5));
  df_view *snapshot = df_query_once(s, spec);
  CHECK(snapshot && snapshot != w->result);
  CHECK(df_store_watch_count(s) == 2);
  CHECK(df_edit_retract(&e, 1, DF_ATTR_TITLE, df_string_z("wrong")));
  CHECK(df_edit_apply(s, &e) == DF_OK && c.calls == 2);
  CHECK(df_edit_set(&e, 1, DF_ATTR_ORDER, df_int(8)));
  CHECK(df_edit_apply(s, &e) == DF_OK && c.calls == 3);
  int64_t n;
  CHECK(df_view_get(snapshot, 1, DF_ATTR_ORDER, &b) == DF_OK);
  CHECK(df_value_int(b, &n) == DF_OK && n == 7);
  /* Set/unset/delete/retract are evaluated in batch order. */
  CHECK(df_edit_unset(&e, 1, DF_ATTR_ORDER));
  CHECK(df_edit_set(&e, 1, DF_ATTR_ORDER, df_int(9)));
  CHECK(df_edit_delete(&e, 1));
  CHECK(df_edit_set(&e, 1, DF_ATTR_ORDER, df_int(10)));
  CHECK(df_edit_apply(s, &e) == DF_OK);
  CHECK(number(s, 1, DF_ATTR_ORDER) == 10);
  CHECK(df_store_get(s, 1, DF_ATTR_TITLE, &b) == DF_ABSENT);
  CHECK(df_edit_set(&e, 1, DF_ATTR_TITLE, df_string_z("restored")));
  CHECK(df_edit_apply(s, &e) == DF_OK);
  /* Counter and fields roll back together on a failed commit. */
  id = df_edit_create(&e, s);
  CHECK(id == 2);
  CHECK(df_edit_set(&e, id, DF_ATTR_ORDER, df_int(20)));
  df_test_commit_failure(s);
  CHECK(df_edit_apply(s, &e) == DF_ERROR);
  CHECK(df_store_get(s, 2, DF_ATTR_ORDER, &b) == DF_ABSENT);
  CHECK(df_edit_create(&e, s) == 2);
  CHECK(df_edit_apply(s, &e) == DF_OK); /* Counter-only create persists. */
  CHECK(df_edit_create(&e, s) == 3);
  df_edit_clear(&e);
  /* Another batch cannot silently take a reserved ID. */
  CHECK(df_edit_create(&e, s) == 3);
  df_edit other = {0};
  CHECK(df_edit_create(&other, s) == 3);
  CHECK(df_edit_apply(s, &other) == DF_OK);
  CHECK(df_edit_apply(s, &e) == DF_ERROR);
  df_edit_free(&other);
  /* Enumeration reuses Datafriend-owned buffers once capacity is sufficient. */
  df_entities ids = {0};
  CHECK(df_view_entities(w->result, DF_ATTR_ORDER, 0, &ids) == DF_OK);
  CHECK(ids.count == 1 && ids.data[0] == 1);
  df_test_allocation_failure(0);
  CHECK(df_view_entities(w->result, DF_ATTR_ORDER, 0, &ids) == DF_OK);
  df_test_allocation_failure(-1);
  df_entities_free(&ids);
  /* Every allocation failure leaves committed values readable through the
   * same retained watch and makes a later edit usable again. */
  bool succeeded = false;
  for (int fail = 0; fail < 160; ++fail) {
    CHECK(df_edit_set(&e, 1, DF_ATTR_ORDER, df_int(100)));
    df_test_allocation_failure(fail);
    df_status status = df_edit_apply(s, &e);
    df_test_allocation_failure(-1);
    CHECK(df_view_valid(w->result));
    CHECK(number(s, 1, DF_ATTR_ORDER) == (status == DF_OK ? 100 : 10));
    if (status == DF_OK) {
      succeeded = true;
      break;
    }
  }
  CHECK(succeeded);
  /* Native joins and constants use the same planner as the wire adapter. */
  CHECK(df_edit_set(&e, 1, DF_ATTR_PARENT, df_entity(5)));
  CHECK(df_edit_set(&e, 5, DF_ATTR_TITLE, df_string_z("parent")));
  CHECK(df_edit_apply(s, &e) == DF_OK);
  df_match join[] = {{df_id(1), DF_ATTR_PARENT, df_var(0), false},
                     {df_var(0), DF_ATTR_TITLE, df_const(df_string_z("parent")), true}};
  df_view *joined = df_query_once(s, (df_query_spec){join, 2, false});
  CHECK(joined);
  CHECK(df_view_get(joined, 5, DF_ATTR_TITLE, &b) == DF_OK);
  df_view_release(joined);
  /* Invalid values fail the entire batch. */
  CHECK(df_edit_set(&e, 1, DF_ATTR_ORDER, df_int(200)));
  CHECK(!df_edit_set(&e, 1, 0, df_int(0)));
  CHECK(df_edit_apply(s, &e) == DF_ERROR && number(s, 1, DF_ATTR_ORDER) == 100);
  df_edit_free(&e);
  df_view_retain(w->result);
  df_view *retained = w->result;
  df_watch_remove(s, &w);
  df_watch_remove(s, &c.added);
  CHECK(df_store_watch_count(s) == 0);
  CHECK(df_store_close(s, &error) == DF_OK);
  df_free(error.data);
  CHECK(df_view_get(retained, 1, DF_ATTR_ORDER, &b) == DF_OK);
  df_view_release(retained);
  df_view_release(snapshot);
  s = df_store_open(&options, &error);
  CHECK(s);
  df_free(error.data);
  CHECK(df_edit_create(&e, s) == 6);
  df_edit_free(&e);
  CHECK(df_store_close(s, &error) == DF_OK);
  df_free(error.data);
  clean(path);
  puts("PASS direct C API: atomic batches, IDs, snapshots, watches, joins, allocation failures");
  return 0;
}
