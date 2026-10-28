/* Exercise the real network state machine with deterministic clock boundaries
 * and a socket pair. No server timing or sleeps determine the outcome. */
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <time.h>
static unsigned long long ticks;
static int queue_clock(clockid_t id, struct timespec *t) {
  (void)id;
  ++ticks;
  t->tv_sec = (time_t)(ticks / 1000000);
  t->tv_nsec = (long)(ticks % 1000000) * 1000;
  return 0;
}
#define clock_gettime queue_clock
#include "../network.c"
#undef clock_gettime
#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); \
} } while (0)

typedef struct fixture {
  char root[256], owner_path[300], replica_path[300];
  df_store *owner, *replica;
  network *n;
  int peer;
  unsigned publications;
} fixture;
static const char key[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static void published(void *p, const uint64_t *ids, size_t count) {
  (void)ids;
  (void)count;
  ++((fixture *)p)->publications;
}
static void create(fixture *f) {
  memset(f, 0, sizeof(*f));
  strcpy(f->root, "/tmp/datafriend-queue-XXXXXX");
  CHECK(mkdtemp(f->root));
  snprintf(f->owner_path, sizeof(f->owner_path), "%s/owner", f->root);
  snprintf(f->replica_path, sizeof(f->replica_path), "%s/replica", f->root);
  df_buffer error = {0};
  df_options options = {f->owner_path, key, DF_BUFFERED, 1 << 24};
  f->owner = df_store_open(&options, &error);
  CHECK(f->owner);
  options.directory = f->replica_path;
  f->replica = df_store_open(&options, &error);
  CHECK(f->replica);
  f->n = df_network_open("127.0.0.1", 7777, &error);
  CHECK(f->n);
  CHECK(df_network_subscribe(f->n, f->replica, key, published, f, &error) == 1);
  df_free(error.data);
  int sockets[2];
  CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
  CHECK(!fcntl(sockets[0], F_SETFL, O_NONBLOCK));
  f->n->fd = sockets[0];
  f->peer = sockets[1];
  f->n->heartbeat_interval = UINT64_MAX;
  f->n->channels[0].subscribed = true;
  f->n->channels[0].epoch = 1;
  df_track(f->owner);
}
static void clean(const char *path) {
  char file[350];
  const char *names[] = {"data.mdb", "lock.mdb", "datafriend.owner"};
  for (size_t i = 0; i < 3; ++i) {
    snprintf(file, sizeof(file), "%s/%s", path, names[i]);
    unlink(file);
  }
  rmdir(path);
}
static void destroy(fixture *f) {
  df_network_close(f->n);
  close(f->peer);
  df_buffer error = {0};
  CHECK(df_store_close(f->owner, &error) == DF_OK);
  CHECK(df_store_close(f->replica, &error) == DF_OK);
  df_free(error.data);
  clean(f->owner_path);
  clean(f->replica_path);
  rmdir(f->root);
}
static df_writer packet(uint32_t request, unsigned status, uint64_t epoch,
                        uint64_t revision, df_bytes payload) {
  df_writer w = {0};
  df_u32(&w, request);
  df_u32(&w, 1);
  df_u8(&w, (uint8_t)status);
  df_u64(&w, epoch);
  df_u64(&w, revision);
  df_append(&w, payload.data, payload.size);
  CHECK(!w.failed);
  return w;
}
static df_writer update(fixture *f, uint32_t id, const char *text) {
  uint64_t before = f->owner->revision;
  df_edit edit = {0};
  CHECK(df_edit_set(&edit, id, DF_ATTR_TITLE, df_string_z(text)));
  CHECK(df_edit_apply(f->owner, &edit) == DF_OK);
  df_edit_free(&edit);
  df_writer delta = {0};
  bool available = false;
  CHECK(df_sync_delta(f->owner, before, &delta, &available) && available);
  df_writer w = packet(0, 5, 1, f->owner->revision, df_written(&delta));
  df_writer_free(&delta);
  df_sync_cancel(f->owner);
  return w;
}
static bool has(fixture *f, unsigned id) {
  df_slice value;
  return df_store_get(f->replica, id, DF_ATTR_TITLE, &value) == DF_OK;
}
static size_t pending(fixture *f) {
  size_t count = 0, bytes = 0;
  for (incoming *b = f->n->channels[0].opening_head; b; b = b->next) {
    ++count;
    bytes += sizeof(*b) + b->size;
  }
  CHECK(bytes == f->n->channels[0].opening_bytes);
  return count;
}
static void ordered_budget(void) {
  fixture f;
  create(&f);
  for (unsigned i = 1; i <= 3; ++i) {
    df_writer w = update(&f, i, "independent change");
    df_blob(&f.n->input, df_written(&w));
    df_writer_free(&w);
  }
  /* Four microseconds allows parsing all three frames, but expires before
   * channel processing. The fake clock advances one microsecond per check. */
  df_network_pump(f.n, 4);
  CHECK(!f.n->input.size && pending(&f) == 3);
  CHECK(!f.publications && !has(&f, 1));
  CHECK(!df_network_settled(f.n) && df_network_prepare(f.n).timeout_us == 0);
  df_network_pump(f.n, 0);
  CHECK(pending(&f) == 3 && !f.publications);
  for (unsigned i = 1; i <= 3; ++i) {
    df_network_pump(f.n, 4);
    CHECK(pending(&f) == 3 - i && f.publications == i);
    CHECK(has(&f, i) && (i == 3 || !has(&f, i + 1)));
    CHECK(f.n->stats.optimistic_publications == i);
    CHECK(!f.n->stats.optimistic_fallbacks && !f.n->stats.restarts && !f.n->stats.requests);
    if (i < 3)
      CHECK(!df_network_settled(f.n) && df_network_prepare(f.n).timeout_us == 0);
  }
  CHECK(df_network_settled(f.n) && df_network_prepare(f.n).timeout_us > 0);
  destroy(&f);
}
static void missing_predecessor(void) {
  fixture f;
  create(&f);
  df_writer a = update(&f, 1, "missing"), b = update(&f, 2, "later");
  CHECK(receive(f.n, df_written(&b)));
  uint64_t before = f.replica->revision;
  df_network_pump(f.n, 4);
  CHECK(f.n->stats.optimistic_fallbacks == 1 && f.n->stats.requests == 1);
  CHECK(f.n->channels[0].state == RECONCILE && f.n->channels[0].request);
  CHECK(f.replica->revision == before && !has(&f, 1) && !has(&f, 2) && !f.publications);
  df_writer_free(&a);
  df_writer_free(&b);
  destroy(&f);
}
static void overflow(void) {
  fixture f;
  create(&f);
  char value[60001];
  memset(value, 'x', sizeof(value) - 1);
  value[sizeof(value) - 1] = 0;
  df_writer a = update(&f, 1, value), b = update(&f, 2, value);
  CHECK(a.size == b.size);
  size_t bytes = sizeof(incoming) + a.size;
  while (f.n->channels[0].opening_bytes <= QUEUE_LIMIT - bytes)
    CHECK(receive(f.n, df_written(&a)));
  CHECK(pending(&f) > 1 && f.n->channels[0].opening_bytes <= QUEUE_LIMIT);
  CHECK(receive(f.n, df_written(&b)));
  CHECK(pending(&f) == 1 && f.n->channels[0].opening_bytes == bytes);
  df_network_pump(f.n, 4);
  CHECK(f.n->stats.optimistic_fallbacks == 1 && !f.publications);
  CHECK(!has(&f, 1) && !has(&f, 2));
  df_writer_free(&a);
  df_writer_free(&b);
  destroy(&f);
}
static void lifecycle(void) {
  fixture f;
  create(&f);
  df_writer a = update(&f, 1, "one"), b = update(&f, 2, "two");
  CHECK(receive(f.n, df_written(&a)) && receive(f.n, df_written(&b)));
  CHECK(pending(&f) == 2);
  /* An older opening cannot replace the newer queued state. */
  df_writer older = packet(0, 3, 1, 0, df_span(NULL, 0));
  CHECK(receive(f.n, df_written(&older)) && pending(&f) == 2);
  df_writer_free(&older);
  /* A new server epoch invalidates the old chain, even at a lower revision. */
  df_writer epoch = packet(0, 3, 2, 0, df_span(NULL, 0));
  CHECK(receive(f.n, df_written(&epoch)) && pending(&f) == 1);
  df_writer_free(&epoch);
  df_network_online(f.n, 0);
  CHECK(!pending(&f) && !f.n->channels[0].opening_tail);
  /* Detach must release an entire chain, not only the last notification. */
  CHECK(receive(f.n, df_written(&a)) && receive(f.n, df_written(&b)));
  df_buffer error = {0};
  CHECK(df_network_detach(f.n, 1, &error) == DF_OK);
  CHECK(!f.n->count && !f.replica->network_owner);
  df_free(error.data);
  df_writer_free(&a);
  df_writer_free(&b);
  destroy(&f);
}
static void in_flight(void) {
  fixture f;
  create(&f);
  channel *c = &f.n->channels[0];
  c->request = 7;
  c->state = FETCH;
  c->sent = c->started = now_us();
  df_writer a = update(&f, 1, "one"), b = update(&f, 2, "two");
  CHECK(receive(f.n, df_written(&a)) && receive(f.n, df_written(&b)));
  df_network_pump(f.n, 4);
  CHECK(pending(&f) == 2 && !f.publications);
  df_writer reply = packet(7, 1, 1, 2, df_span(NULL, 0));
  CHECK(receive(f.n, df_written(&reply)));
  CHECK(!c->request && !c->superseded && pending(&f) == 2);
  df_network_pump(f.n, 4);
  df_network_pump(f.n, 4);
  CHECK(has(&f, 1) && has(&f, 2) && f.publications == 2);
  CHECK(!f.n->stats.optimistic_fallbacks && f.n->stats.restarts == 1);
  df_writer_free(&a);
  df_writer_free(&b);
  df_writer_free(&reply);
  destroy(&f);
}
int main(void) {
  ordered_budget();
  missing_predecessor();
  overflow();
  lifecycle();
  in_flight();
  puts("PASS: ordered subscriber deltas, pump budgets, fingerprint fallback and queue lifecycle");
  return 0;
}
