/* Run against the production server via scripts/test_native_sync.py. */
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include "datafriend/network.h"
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
enum { CHANNELS = 140, INITIAL = 8 };
typedef struct test_state {
  void *network, *stores[CHANNELS];
  char keys[CHANNELS][65];
  bool grow, grown;
  size_t publications;
} test_state;
static void require(bool ok, const char *message) {
  if (!ok) {
    fprintf(stderr, "%s\n", message);
    abort();
  }
}
static void published(void *context, const uint64_t *views, size_t count) {
  test_state *s = context;
  (void)views;
  (void)count;
  ++s->publications;
  if (!s->grow || s->grown)
    return;
  s->grown = true;
  df_buffer error = {0};
  /* The initial eight slots are full. Fail growth once, then retry without
   * consuming an ID or disturbing any existing channel. */
  df_test_allocation_failure(0);
  require(
      !df_network_subscribe(s->network, s->stores[INITIAL], s->keys[INITIAL], NULL, NULL, &error),
      "channel allocation failure was ignored");
  df_test_allocation_failure(-1);
  df_free(error.data);
  for (size_t i = INITIAL; i < CHANNELS; ++i) {
    uint32_t id = df_network_subscribe(s->network, s->stores[i], s->keys[i], NULL, NULL, &error);
    require(id == i + 1, "callback growth changed channel IDs");
    df_free(error.data);
  }
}
static void watched(void *context, df_view *result) {
  require(df_view_valid(result), "invalid published view");
  published(context, NULL, 0);
}
static void *store(const char *path, const char *key) {
  df_buffer error = {0};
  void *s = df_open_sized((const uint8_t *)path, strlen(path), (const uint8_t *)key, 64, 0, 1 << 20,
                          &error);
  if (!s)
    fprintf(stderr, "%.*s\n", (int)error.size, error.data);
  require(s != NULL, "open test namespace");
  df_free(error.data);
  return s;
}
static void edit(void *owner, const char *text) {
  df_writer w = {0};
  df_buffer reply = {0};
  df_u8(&w, 1);
  df_u8(&w, 10);
  df_u32(&w, 0);
  df_u32(&w, 1);
  df_u64(&w, 1);
  df_blob(&w, df_text("title"));
  df_blob(&w, df_text(text));
  require(!w.failed && !df_call(owner, w.data, w.size, &reply), "edit owner");
  df_writer_free(&w);
  df_free(reply.data);
}
static bool equal(void *a, void *b) {
  uint8_t request[] = {1, 12};
  df_buffer x = {0}, y = {0};
  require(!df_call(a, request, sizeof(request), &x) && !df_call(b, request, sizeof(request), &y),
          "export facts");
  bool same = x.size == y.size && !memcmp(x.data, y.data, x.size);
  df_free(x.data);
  df_free(y.data);
  return same;
}
static uint64_t now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}
static void wait_for(void *writer, void *owner, test_state *s, bool grown) {
  uint64_t deadline = now() + 30000000;
  void *networks[] = {writer, s->network};
  bool runnable[] = {true, true};
  do {
    /* No periodic pump: only readiness, deadlines, or the initial local edit.
     * A small budget also exercises continuation after partial processing. */
    for (size_t i = 0; i < 2; ++i)
      if (runnable[i])
        df_network_pump(networks[i], 50);
    if (df_network_settled(writer) && df_network_settled(s->network) &&
        equal(owner, s->stores[0]) &&
        (!grown || (s->grown && equal(owner, s->stores[CHANNELS - 1]))))
      return;
    struct pollfd fds[2];
    df_network_watch watches[2];
    uint64_t before = now();
    if (before >= deadline)
      break;
    uint64_t delay = deadline - before;
    for (size_t i = 0; i < 2; ++i) {
      watches[i] = df_network_prepare(networks[i]);
      fds[i] = (struct pollfd){watches[i].fd, 0, 0};
      if (watches[i].events & DF_NETWORK_READ)
        fds[i].events |= POLLIN;
      if (watches[i].events & DF_NETWORK_WRITE)
        fds[i].events |= POLLOUT;
      if (watches[i].timeout_us < delay)
        delay = watches[i].timeout_us;
    }
    int result = poll(fds, 2, (int)((delay + 999) / 1000));
    require(result >= 0 || errno == EINTR, "readiness poll failed");
    uint64_t elapsed = now() - before;
    for (size_t i = 0; i < 2; ++i)
      runnable[i] = fds[i].revents || watches[i].timeout_us <= elapsed;
  } while (now() < deadline);
  fprintf(stderr, "writer: %s; reader: %s\n", df_network_error(writer),
          df_network_error(s->network));
  require(false, "dynamic channel sync timed out");
}
static void clean(const char *directory) {
  char path[512];
  const char *files[] = {"data.mdb", "lock.mdb", "datafriend.owner"};
  for (size_t i = 0; i < 3; ++i) {
    snprintf(path, sizeof(path), "%s/%s", directory, files[i]);
    unlink(path);
  }
  rmdir(directory);
}
static void retry_test(void *replica) {
  /* A peer that accepts then disconnects must leave a reconnect deadline,
   * even when no render frame will call pump again. */
  int listener = socket(AF_INET, SOCK_STREAM, 0);
  require(listener >= 0, "retry listener");
  struct sockaddr_in address = {0};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  require(!bind(listener, (struct sockaddr *)&address, sizeof(address)) && !listen(listener, 1),
          "retry listener bind");
  socklen_t size = sizeof(address);
  require(!getsockname(listener, (struct sockaddr *)&address, &size), "retry listener address");
  df_buffer error = {0};
  df_network *n = df_network_open("127.0.0.1", ntohs(address.sin_port), &error);
  require(n && df_network_follow(n, replica, &error), "retry subscription");
  df_network_pump(n, 1500);
  struct pollfd incoming = {listener, POLLIN, 0};
  require(poll(&incoming, 1, 1000) == 1, "retry connection did not arrive");
  int peer = accept(listener, NULL, NULL);
  require(peer >= 0, "retry accept");
  close(peer);
  close(listener);
  df_network_watch w = df_network_prepare(n);
  struct pollfd closed = {w.fd, POLLIN | POLLOUT, 0};
  require(poll(&closed, 1, 1000) == 1, "peer close not observable");
  df_network_pump(n, 1500);
  w = df_network_prepare(n);
  require(w.fd == -1 && !w.events && w.timeout_us > 0 && w.timeout_us <= 500000,
          "failed connection needs a bounded reconnect deadline");
  df_network_close(n);
  df_free(error.data);
}
int main(int argc, char **argv) {
  require(argc == 3, "usage: channel_test HOST PORT");
  char root[] = "/tmp/datafriend-channels-XXXXXX", path[256], key[65];
  require(mkdtemp(root) != NULL, "temporary directory");
  df_buffer error = {0};
  void *identity = df_identity_open((const uint8_t *)"channels", 8, 1, &error);
  require(identity != NULL, "test identity");
  df_free(error.data);
  const uint8_t *public = df_identity_public(identity);
  for (unsigned i = 0; i < 32; ++i)
    snprintf(key + i * 2, 3, "%02x", public[i]);
  snprintf(path, sizeof(path), "%s/owner", root);
  void *owner = store(path, key);
  test_state s = {0};
  for (unsigned i = 0; i < CHANNELS; ++i) {
    snprintf(s.keys[i], sizeof(s.keys[i]), "%064x", i + 1000);
    if (i == 0 || i == CHANNELS - 1)
      memcpy(s.keys[i], key, sizeof(key));
    snprintf(path, sizeof(path), "%s/peer-%u", root, i);
    s.stores[i] = store(path, s.keys[i]);
  }
  void *writer = df_network_open(argv[1], (uint16_t)atoi(argv[2]), &error);
  require(writer != NULL, "writer network");
  df_free(error.data);
  s.network = df_network_open(argv[1], (uint16_t)atoi(argv[2]), &error);
  require(s.network != NULL, "subscriber network");
  df_free(error.data);
  df_network_watch idle = df_network_prepare(s.network);
  require(idle.fd == -1 && !idle.events && idle.timeout_us == UINT64_MAX,
          "empty network must not wake");
  require(df_network_push(writer, owner, identity, &error) == 1, "push owner");
  df_free(error.data);
  df_match title = {df_id(1), DF_ATTR_TITLE, df_var(0), true};
  df_watch *watch = df_watch_query(s.stores[0], (df_query_spec){&title, 1, false}, watched, &s);
  require(watch != NULL, "native publication watch");
  for (unsigned i = 0; i < INITIAL; ++i) {
    require(df_network_follow(s.network, s.stores[i], &error) == i + 1, "initial channel IDs");
    df_free(error.data);
  }
  edit(owner, "baseline");
  wait_for(writer, owner, &s, false);
  idle = df_network_prepare(s.network);
  require(idle.fd >= 0 && idle.events == DF_NETWORK_READ && idle.timeout_us > 0,
          "settled subscriber must wait for reads or a heartbeat");
  s.grow = true;
  edit(owner, "grow from unsolicited publication");
  require(df_network_prepare(writer).timeout_us == 0, "local revision must run immediately");
  wait_for(writer, owner, &s, true);
  require(df_network_statistics(s.network)->connections == 1, "growth reconnected the socket");
  /* Late channels must receive updates too, and retain IDs across reconnect. */
  edit(owner, "late channel update");
  wait_for(writer, owner, &s, true);
  df_network_online(s.network, 0);
  idle = df_network_prepare(s.network);
  require(idle.fd == -1 && !idle.events && idle.timeout_us == UINT64_MAX,
          "offline network must not wake");
  edit(owner, "offline update");
  df_network_online(s.network, 1);
  require(df_network_prepare(s.network).timeout_us == 0, "reconnect must run immediately");
  wait_for(writer, owner, &s, true);
  require(df_network_statistics(s.network)->connections == 2, "reconnect not exercised");
  require(s.publications >= 3, "missing publications");
  /* Detach a middle slot while the final, live replica is moved into it. */
  df_test_allocation_failure(0);
  require(df_network_detach(s.network, 2, &error) == DF_ERROR, "detach allocation rollback");
  df_test_allocation_failure(-1);
  df_free(error.data);
  require(df_network_detach(s.network, 2, &error) == DF_OK, "middle detach");
  df_free(error.data);
  require(df_store_close(s.stores[1], &error) == DF_OK, "detached store can close");
  df_free(error.data);
  s.stores[1] = NULL;
  edit(owner, "update after middle detach");
  wait_for(writer, owner, &s, true);
  require(df_network_statistics(s.network)->connections == 2, "detach reconnected other channels");
  /* Reattach gets a fresh ID; replies to the retired ID cannot hit the new store. */
  snprintf(path, sizeof(path), "%s/peer-1", root);
  s.stores[1] = store(path, s.keys[1]);
  require(df_network_follow(s.network, s.stores[1], &error) == CHANNELS + 1, "fresh channel ID");
  df_free(error.data);
  df_network_pump(s.network, 500);
  require(df_network_detach(s.network, CHANNELS + 1, &error) == DF_OK,
          "detach in-flight subscribe");
  df_free(error.data);
  require(df_store_close(s.stores[1], &error) == DF_OK, "close in-flight detached store");
  df_free(error.data);
  s.stores[1] = NULL;
  edit(owner, "update after in-flight detach");
  wait_for(writer, owner, &s, true);
  df_watch_remove(s.stores[0], &watch);
  df_network_close(s.network);
  df_network_close(writer);
  retry_test(s.stores[0]);
  for (unsigned i = 0; i < CHANNELS; ++i) {
    df_close(s.stores[i]);
    snprintf(path, sizeof(path), "%s/peer-%u", root, i);
    clean(path);
  }
  df_close(owner);
  snprintf(path, sizeof(path), "%s/owner", root);
  clean(path);
  df_identity_close(identity);
  rmdir(root);
  puts("PASS dynamic channels: 140 channels, callback growth, allocation failure, stable IDs, "
       "late updates, reconnect, native watches, middle/in-flight detach");
  return 0;
}
