#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "datafriend/network.h"
#include "store.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
enum { FRAME_LIMIT = 8 * 1024 * 1024, QUEUE_LIMIT = 2 * FRAME_LIMIT };
typedef enum { IDLE, START, RECONCILE, APPLY, SUBSCRIBE, FETCH } phase;
typedef struct response {
  uint32_t request, channel;
  uint8_t status;
  uint64_t epoch, revision;
  df_bytes challenge, payload;
} response;
typedef struct channel {
  df_store *store;
  uint32_t id;
  void *signer;
  uint8_t key[32];
  df_publication publish;
  void *context;
  phase state;
  uint32_t request;
  uint64_t sent, started;
  uint64_t epoch, revision, sequence, pushed, snapshot;
  uint8_t challenge[32];
  bool challenged, absent, subscribed, final_page, superseded;
  unsigned rounds;
  df_writer opening;
} channel;
typedef struct outgoing {
  struct outgoing *next;
  df_writer data;
  uint32_t request;
} outgoing;
struct df_network {
  struct sockaddr_storage address;
  socklen_t address_size;
  channel *channels;
  size_t count, capacity, next_channel;
  ct_map channel_index;
  uint32_t next_channel_id;
  bool pumping;
  int fd;
  bool connecting, enabled, measuring;
  uint32_t next_request, heartbeat;
  df_writer input;
  outgoing *head, *tail;
  size_t offset, queued;
  ct_map timings;
  df_request_timing *samples;
  size_t sample_count;
  uint64_t retry, connected_at, last_ping, heartbeat_sent, heartbeat_interval;
  unsigned failures;
  char error[256];
  df_network_stats stats;
};
typedef df_network network;
static uint64_t now_us(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}
static bool error(network *n, const char *message) {
  snprintf(n->error, sizeof(n->error), "%s", message ? message : "Datafriend network failure");
  return false;
}
static bool memory(network *n, bool ok) { return ok || error(n, "out of memory"); }
static bool native(network *n, channel *c, unsigned op, df_bytes packet, df_writer *out) {
  uint64_t start = now_us();
  bool ok = df_sync(c->store, op, packet, out);
  n->stats.reconcile_us += now_us() - start;
  return ok || error(n, c->store->error);
}
static void reset(network *n) {
  if (n->fd >= 0)
    close(n->fd);
  n->fd = -1;
  n->connecting = false;
  n->heartbeat = 0;
  n->input.size = 0;
  n->input.failed = false;
  while (n->head) {
    outgoing *b = n->head;
    n->head = b->next;
    df_writer_free(&b->data);
    free(b);
  }
  n->tail = NULL;
  for (size_t i = 0; i < n->timings.size; ++i)
    free((void *)n->timings.items[i].value);
  ct_map_clear(&n->timings);
  n->offset = n->queued = 0;
  for (size_t i = 0; i < n->count; ++i) {
    channel *c = n->channels + i;
    df_sync_cancel(c->store);
    c->state = IDLE;
    c->request = 0;
    c->pushed = UINT64_MAX;
    c->subscribed = false;
    c->opening.size = 0;
    c->opening.failed = false;
    c->superseded = false; /* Challenges are reset at the ordered start barrier. */
  }
}
static void fail(network *n) {
  reset(n);
  unsigned shift = n->failures < 7 ? n->failures : 7;
  if (n->failures < UINT_MAX)
    ++n->failures;
  n->retry = now_us() + (UINT64_C(250000) << shift);
}
static bool connect_socket(network *n) {
  n->fd = socket(n->address.ss_family, SOCK_STREAM, 0);
  if (n->fd < 0)
    return error(n, "socket failed");
  if (fcntl(n->fd, F_SETFL, O_NONBLOCK) < 0)
    return error(n, "nonblocking socket failed");
  int yes = 1;
  setsockopt(n->fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
#ifdef SO_NOSIGPIPE
  setsockopt(n->fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
  n->connected_at = now_us();
  int rc = connect(n->fd, (struct sockaddr *)&n->address, n->address_size);
  if (rc < 0 && errno != EINPROGRESS)
    return error(n, strerror(errno));
  n->connecting = true;
  return true;
}
static bool send_request(network *n, uint32_t id, channel *c, uint8_t op, df_bytes payload,
                         uint32_t *request_out) {
  if (!n->next_request)
    return error(n, "request counter exhausted");
  uint32_t request = n->next_request++;
  df_writer w = {0};
  outgoing *frame = NULL;
  bool ok = false;
  df_append(&w, "LBS\5", 4);
  df_u32(&w, request);
  df_u32(&w, id);
  df_u8(&w, op);
  df_blob(&w, c ? df_span(c->key, 32) : df_span(NULL, 0));
  df_blob(&w, df_text(DATAFRIEND_SCHEMA));
  df_u64(&w, c ? c->epoch : 0);
  df_u64(&w, c ? c->revision : 0);
  df_blob(&w, payload);
  if (op == 3 || op == 6) {
    if (!c || !c->signer || !c->challenged) {
      error(n, "missing owner challenge");
      goto done;
    }
    df_append(&w, c->challenge, 32);
    df_u64(&w, c->sequence++);
    if (!memory(n, !w.failed))
      goto done;
    uint64_t start = now_us();
    df_buffer signature = {0};
    int rc = df_identity_sign(c->signer, w.data, w.size, &signature);
    n->stats.sign_us += now_us() - start;
    if (rc) {
      free(signature.data);
      error(n, "owner signing failed");
      goto done;
    }
    df_append(&w, signature.data, signature.size);
    free(signature.data);
    ++n->stats.signed_batches;
  }
  if (!memory(n, !w.failed))
    goto done;
  if (w.size > FRAME_LIMIT || n->queued > QUEUE_LIMIT - 4 - w.size) {
    error(n, "network frame or queue limit");
    goto done;
  }
  frame = calloc(1, sizeof(*frame));
  if (!memory(n, frame != NULL))
    goto done;
  frame->request = request;
  df_blob(&frame->data, df_written(&w));
  if (!memory(n, !frame->data.failed))
    goto done;
  if (n->measuring && n->sample_count + n->timings.size < 4096) {
    df_request_timing *sample = calloc(1, sizeof(*sample));
    if (!memory(n, sample != NULL))
      goto done;
    sample->request = request;
    sample->channel = id;
    sample->operation = op;
    sample->snapshot_revision = c && c->signer ? c->snapshot : 0;
    sample->enqueued_us = now_us();
    if (!ct_map_put(&n->timings, request, (uintptr_t)sample)) {
      free(sample);
      error(n, "out of memory");
      goto done;
    }
  }
  n->queued += frame->data.size;
  if (n->tail)
    n->tail->next = frame;
  else
    n->head = frame;
  n->tail = frame;
  frame = NULL;
  ++n->stats.requests;
  if (c && op != 6) {
    c->request = request;
    c->sent = now_us();
  }
  if (request_out)
    *request_out = request;
  ok = true;
done:
  if (frame) {
    df_writer_free(&frame->data);
    free(frame);
  }
  df_writer_free(&w);
  return ok;
}
static bool parse(network *n, df_bytes b, response *out) {
  df_reader r = {b, false};
  out->request = df_read32(&r);
  out->channel = df_read32(&r);
  unsigned flags = df_read8(&r);
  out->status = flags & 127;
  out->epoch = df_read64(&r);
  out->revision = df_read64(&r);
  out->challenge = flags & 128 ? df_take(&r, 32) : df_span(NULL, 0);
  out->payload = r.data;
  return !r.failed || error(n, "invalid response");
}
static void finish(network *n, channel *c) {
  df_sync_cancel(c->store);
  c->state = IDLE;
  if (c->signer)
    c->pushed = c->snapshot;
  ++n->stats.exchanges;
  n->failures = 0;
  n->error[0] = 0;
}
static bool transfer(network *n, uint32_t id, channel *c) {
  df_writer w = {0};
  bool ok = false;
  if (c->signer) {
    if (!native(n, c, 2, df_span(NULL, 0), &w))
      goto done;
    df_reader r = {df_written(&w), false};
    if (!df_read32(&r) && !c->absent) {
      finish(n, c);
      ok = true;
      goto done;
    }
    c->state = APPLY;
    ok = send_request(n, id, c, 3, df_written(&w), NULL);
  } else {
    if (!native(n, c, 3, df_span(NULL, 0), &w))
      goto done;
    c->final_page = w.data[0] == 1;
    c->state = FETCH;
    ok = send_request(n, id, c, 2, df_span(w.data + 1, w.size - 1), NULL);
  }
done:
  df_writer_free(&w);
  return ok;
}
static bool reconcile(network *n, uint32_t id, channel *c, df_bytes packet) {
  if (packet.size == 4)
    return transfer(n, id, c);
  if (++c->rounds > 128)
    return error(n, "reconciliation round limit");
  c->state = RECONCILE;
  return send_request(n, id, c, 1, packet, NULL);
}
static bool native_round(network *n, uint32_t id, channel *c, unsigned op, df_bytes packet) {
  df_writer w = {0};
  bool ok = native(n, c, op, packet, &w) && reconcile(n, id, c, df_written(&w));
  df_writer_free(&w);
  return ok;
}
static bool publish(network *n, channel *c, df_bytes bytes) {
  /* Watch callbacks can append channels and relocate c. Capture everything
   * needed before dispatch; publication is the final action of an apply. */
  df_store *store = c->store;
  df_publication callback = c->publish;
  void *context = c->context;
  df_notify(store, bytes);
  if (!callback)
    return true;
  df_reader r = {bytes, false};
  uint32_t count = df_count(&r, 8);
  uint64_t local[64], *ids = count <= 64 ? local : malloc((size_t)count * sizeof(*ids));
  if (!memory(n, ids != NULL))
    return false;
  for (uint32_t i = 0; i < count; ++i)
    ids[i] = df_read64(&r);
  bool ok = df_end(&r);
  if (ok)
    callback(context, ids, count);
  else
    error(n, "invalid publication");
  if (ids != local)
    free(ids);
  return ok;
}
static bool open_response(network *n, uint32_t id, channel *c, response r) {
  if (r.challenge.size)
    return error(n, "challenge on replica");
  df_sync_cancel(c->store);
  c->state = IDLE;
  c->superseded = false;
  if (r.status == 3 || (r.status == 1 && !r.payload.size))
    return true;
  c->epoch = r.epoch;
  c->revision = r.revision;
  c->started = now_us();
  c->rounds = 0;
  if (r.status == 5) {
    df_writer changes = {0};
    bool mismatch = false;
    uint64_t start = now_us();
    bool ok = df_sync_apply_delta(c->store, r.payload, &changes, &mismatch);
    n->stats.apply_us += now_us() - start;
    if (!ok) {
      df_writer_free(&changes);
      error(n, c->store->error);
      df_notify_reset(c->store);
      return false;
    }
    if (!mismatch) {
      ++n->stats.optimistic_publications;
      finish(n, c);
      ok = publish(n, c, df_written(&changes));
      df_writer_free(&changes);
      return ok;
    }
    df_writer_free(&changes);
    ++n->stats.optimistic_fallbacks;
    return native_round(n, id, c, 0, df_span(NULL, 0));
  }
  if (r.status != 0 && r.status != 1 && r.status != 4)
    return error(n, "invalid subscription opening");
  return native_round(n, id, c, 5, r.payload);
}
static bool save_opening(network *n, channel *c, df_bytes packet) {
  c->opening.size = 0;
  df_append(&c->opening, packet.data, packet.size);
  return memory(n, !c->opening.failed);
}
static bool receive(network *n, df_bytes packet) {
  response r;
  if (!parse(n, packet, &r))
    return false;
  ct_pair *timing = ct_map_get(&n->timings, r.request);
  if (timing) {
    df_request_timing *sample = (df_request_timing *)timing->value;
    sample->elapsed_us = now_us() - sample->enqueued_us;
    n->samples[n->sample_count++] = *sample;
    free(sample);
    ct_map_erase(&n->timings, r.request);
  }
  if (!r.channel) {
    if (r.request != n->heartbeat || r.status)
      return error(n, "invalid heartbeat");
    n->heartbeat = 0;
    return true;
  }
  ct_pair *entry = ct_map_get(&n->channel_index, r.channel);
  if (!entry) {
    /* IDs are never reused. Ignore already queued replies for detached stores. */
    if (!n->next_channel_id || r.channel < n->next_channel_id)
      return true;
    return error(n, "unknown channel");
  }
  channel *c = n->channels + entry->value;
  if (!r.request) {
    if (c->signer) {
      if (r.status != 6)
        return error(n, "Littlebear rejected streamed update");
      ++n->stats.optimistic_fallbacks;
      if (!c->request) {
        df_sync_cancel(c->store);
        c->state = IDLE;
        c->pushed = UINT64_MAX;
      }
      return true;
    }
    if (r.status != 3 && r.status != 4 && r.status != 5)
      return error(n, "invalid unsolicited opening");
    if (c->opening.size) {
      response old;
      if (!parse(n, df_written(&c->opening), &old))
        return false;
      if (old.epoch == r.epoch && old.revision > r.revision)
        return true;
    }
    if (!save_opening(n, c, packet))
      return false;
    if (c->request)
      c->superseded = true;
    return true;
  }
  if (r.request != c->request)
    return error(n, "reply request mismatch");
  c->request = 0;
  n->stats.request_us += now_us() - c->sent;
  ++n->stats.request_count;
  if (c->superseded) {
    df_sync_cancel(c->store);
    c->state = IDLE;
    c->superseded = false;
    ++n->stats.restarts;
    return true;
  }
  if (!c->signer && c->state == SUBSCRIBE) {
    c->subscribed = true;
    return open_response(n, r.channel, c, r);
  }
  if (!c->signer &&
      (r.status == 1 || r.status == 3 || r.epoch != c->epoch || r.revision != c->revision)) {
    ++n->stats.restarts;
    df_sync_cancel(c->store);
    c->state = IDLE;
    if (r.status == 3 || r.payload.size)
      return save_opening(n, c, packet);
    return true;
  }
  if (c->signer && r.status == 1) {
    ++n->stats.restarts;
    df_sync_cancel(c->store);
    c->state = IDLE;
    return true;
  }
  if (r.status && !(c->state == START && r.status == 3))
    return error(n, "Littlebear rejected request");
  if (r.challenge.size) {
    if (!c->signer || c->challenged ||
        !((c->state == START && r.status == 3) || c->state == RECONCILE))
      return error(n, "unexpected write challenge");
    memcpy(c->challenge, r.challenge.data, 32);
    c->challenged = true;
  }
  c->epoch = r.epoch;
  c->revision = r.revision;
  switch (c->state) {
  case START:
    c->absent = r.status == 3;
    c->snapshot = c->store->revision;
    return native_round(n, r.channel, c, 0, df_span(NULL, 0));
  case RECONCILE:
    return native_round(n, r.channel, c, 1, r.payload);
  case APPLY:
    c->absent = false;
    return transfer(n, r.channel, c);
  case FETCH: {
    df_writer changes = {0};
    uint64_t start = now_us();
    bool ok = df_sync_apply(c->store, r.payload, &changes);
    n->stats.apply_us += now_us() - start;
    if (!ok) {
      df_writer_free(&changes);
      error(n, c->store->error);
      df_notify_reset(c->store);
      return false;
    }
    if (c->final_page) {
      finish(n, c);
      ok = publish(n, c, df_written(&changes));
    } else
      ok = transfer(n, r.channel, c);
    df_writer_free(&changes);
    return ok;
  }
  default:
    return error(n, "unexpected response phase");
  }
}
static bool io(network *n, uint64_t deadline) {
  while (n->head && now_us() < deadline) {
    outgoing *b = n->head;
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    ssize_t count = send(n->fd, b->data.data + n->offset, b->data.size - n->offset, flags);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      break;
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return error(n, "network send failed");
    n->stats.sent += (uint64_t)count;
    n->queued -= (size_t)count;
    n->offset += (size_t)count;
    if (n->offset == b->data.size) {
      ct_pair *p = ct_map_get(&n->timings, b->request);
      if (p) {
        df_request_timing *sample = (df_request_timing *)p->value;
        sample->sent_us = now_us();
        sample->queue_us = sample->sent_us - sample->enqueued_us;
        if (sample->operation == 6) {
          n->samples[n->sample_count++] = *sample;
          free(sample);
          ct_map_erase(&n->timings, b->request);
        }
      }
      n->head = b->next;
      if (!n->head)
        n->tail = NULL;
      df_writer_free(&b->data);
      free(b);
      n->offset = 0;
    }
  }
  while (now_us() < deadline) {
    if (n->input.size >= 4) {
      df_reader r = {df_written(&n->input), false};
      uint32_t size = df_read32(&r);
      if (size < 25 || size > FRAME_LIMIT)
        return error(n, "invalid frame length");
      if (n->input.size >= size + 4) {
        if (!receive(n, df_span(n->input.data + 4, size)))
          return false;
        n->input.size -= size + 4;
        if (n->input.size)
          memmove(n->input.data, n->input.data + size + 4, n->input.size);
        continue;
      }
    }
    uint8_t bytes[16384];
    ssize_t count = recv(n->fd, bytes, sizeof(bytes), 0);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      break;
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return error(n, "network disconnected");
    n->stats.received += (uint64_t)count;
    df_append(&n->input, bytes, (size_t)count);
    if (!memory(n, !n->input.failed))
      return false;
    if (n->input.size > FRAME_LIMIT + 16384)
      return error(n, "receive buffer limit");
  }
  return true;
}
static bool pump(network *n, uint64_t budget) {
  if (!n->enabled || !n->count)
    return true;
  uint64_t now = now_us(), deadline = budget > UINT64_MAX - now ? UINT64_MAX : now + budget;
  if (n->fd < 0) {
    if (now < n->retry)
      return true;
    if (!connect_socket(n))
      return false;
  }
  if (n->connecting) {
    struct pollfd p = {n->fd, POLLOUT, 0};
    int rc = poll(&p, 1, 0);
    if (rc < 0)
      return error(n, "connect poll failed");
    if (!rc)
      return now >= n->connected_at && now - n->connected_at > 10000000
                 ? error(n, "connect timeout")
                 : true;
    int e = 0;
    socklen_t size = sizeof(e);
    if (getsockopt(n->fd, SOL_SOCKET, SO_ERROR, &e, &size) || e)
      return error(n, "connection refused/unavailable");
    n->connecting = false;
    ++n->stats.connections;
    n->last_ping = now;
    n->error[0] = 0;
  }
  if (!io(n, deadline))
    return false;
  /* Visit the channels present at entry once, resuming after the last visited
   * channel next pump. Publication callbacks may append channels. */
  size_t count = n->count;
  for (size_t visited = 0; visited < count; ++visited) {
    if (now_us() >= deadline)
      break;
    size_t i = n->next_channel;
    n->next_channel = (i + 1) % count;
    channel *c = n->channels + i;
    uint32_t id = c->id;
    if (c->request) {
      if ((now >= c->sent && now - c->sent > 10000000) ||
          (now >= c->started && now - c->started > 30000000))
        return error(n, "sync timeout");
      continue;
    }
    if (!c->signer && c->opening.size) {
      response r;
      /* Detach before publishing: a callback may grow and relocate channels. */
      df_writer opening = c->opening;
      c->opening = (df_writer){0};
      bool ok = parse(n, df_written(&opening), &r) && open_response(n, id, c, r);
      ct_buffer_clear(&opening);
      n->channels[i].opening = opening;
      if (!ok)
        return false;
      continue;
    }
    if (c->state != IDLE)
      continue;
    if (c->signer && c->store->revision != c->pushed) {
      if (n->queued > QUEUE_LIMIT - 512 * 1024)
        continue;
      if (c->challenged) {
        df_writer delta = {0};
        bool available = false, ok = df_sync_delta(c->store, c->pushed, &delta, &available);
        if (!ok) {
          df_writer_free(&delta);
          return error(n, c->store->error);
        }
        if (available) {
          c->snapshot = c->store->revision;
          ok = send_request(n, id, c, 6, df_written(&delta), NULL);
          df_writer_free(&delta);
          if (!ok)
            return false;
          ++n->stats.optimistic_uploads;
          finish(n, c);
          continue;
        }
        df_writer_free(&delta);
      }
      c->epoch = c->revision = c->sequence = 0;
      c->challenged = false;
      c->rounds = 0;
      c->started = now;
      c->state = START;
      uint8_t owner = 1;
      if (!send_request(n, id, c, 0, df_span(&owner, 1), NULL))
        return false;
    } else if (!c->signer && !c->subscribed) {
      c->epoch = c->revision = 0;
      c->started = now;
      c->state = SUBSCRIBE;
      if (!send_request(n, id, c, 8, df_span(NULL, 0), NULL))
        return false;
    }
  }
  if (n->heartbeat && now >= n->heartbeat_sent && now - n->heartbeat_sent > 10000000)
    return error(n, "heartbeat timeout");
  if (!n->heartbeat && now >= n->last_ping && now - n->last_ping > n->heartbeat_interval) {
    if (!send_request(n, 0, NULL, 7, df_span(NULL, 0), &n->heartbeat))
      return false;
    n->heartbeat_sent = n->last_ping = now;
  }
  return io(n, deadline);
}
df_network *df_network_open(const char *host, uint16_t port, df_buffer *out) {
  *out = (df_buffer){0};
  if (!host || !port) {
    df_error(out, "invalid address or port");
    return NULL;
  }
  network *n = calloc(1, sizeof(*n));
  if (!n) {
    df_error(out, "out of memory");
    return NULL;
  }
  n->fd = -1;
  n->enabled = true;
  n->next_request = 1;
  n->next_channel_id = 1;
  n->heartbeat_interval = 10000000;
  struct sockaddr_in *a = (struct sockaddr_in *)&n->address;
  a->sin_family = AF_INET;
  a->sin_port = htons(port);
  if (inet_pton(AF_INET, host, &a->sin_addr) == 1)
    n->address_size = sizeof(*a);
  else {
    struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&n->address;
    memset(a6, 0, sizeof(*a6));
    a6->sin6_family = AF_INET6;
    a6->sin6_port = htons(port);
    if (inet_pton(AF_INET6, host, &a6->sin6_addr) != 1) {
      free(n);
      df_error(out, "network endpoint must be a numeric IP address");
      return NULL;
    }
    n->address_size = sizeof(*a6);
  }
  return n;
}
void df_network_close(void *p) {
  network *n = p;
  if (!n)
    return;
  reset(n);
  df_writer_free(&n->input);
  ct_map_free(&n->timings);
  for (size_t i = 0; i < n->count; ++i) {
    n->channels[i].store->network_owner = NULL;
    df_writer_free(&n->channels[i].opening);
  }
  ct_map_free(&n->channel_index);
  free(n->channels);
  free(n->samples);
  free(n);
}
static uint32_t attach(network *n, df_store *s, void *signer, const char *hex,
                       df_publication callback, void *context, df_buffer *out) {
  *out = (df_buffer){0};
  const char *message = "invalid channel";
  if (!n || !s || !hex)
    goto fail;
  if (!n->next_channel_id) {
    message = "channel IDs exhausted";
    goto fail;
  }
  if (s->network_owner) {
    message = "store already attached";
    goto fail;
  }
  if (strlen(hex) != 64 || s->scope->name_size != 64 || memcmp(s->scope->name, hex, 64)) {
    message = "store namespace must match public key";
    goto fail;
  }
  uint8_t key[32];
  for (unsigned i = 0; i < 64; i += 2) {
    unsigned v = 0;
    for (unsigned j = i; j < i + 2; ++j) {
      char c = hex[j];
      if (c >= '0' && c <= '9')
        v = v * 16 + (unsigned)(c - '0');
      else if (c >= 'a' && c <= 'f')
        v = v * 16 + (unsigned)(c - 'a' + 10);
      else {
        message = "invalid public key";
        goto fail;
      }
    }
    key[i / 2] = (uint8_t)v;
  }
  if (!ct_array_reserve_limit(&n->channels, &n->capacity, n->count + 1, sizeof(*n->channels),
                              UINT32_MAX)) {
    message = "out of memory";
    goto fail;
  }
  if (!ct_map_put(&n->channel_index, n->next_channel_id, n->count)) {
    message = "out of memory";
    goto fail;
  }
  channel *c = n->channels + n->count;
  memset(c, 0, sizeof(*c));
  c->store = s;
  s->network_owner = n;
  c->id = n->next_channel_id++;
  c->signer = signer;
  c->publish = callback;
  c->context = context;
  c->pushed = UINT64_MAX;
  memcpy(c->key, key, 32);
  if (signer)
    df_track(s);
  ++n->count;
  return c->id;
fail:
  df_error(out, message);
  return 0;
}
uint32_t df_network_push(void *n, void *store, void *identity, df_buffer *out) {
  const uint8_t *key = df_identity_public(identity);
  if (!key) {
    *out = (df_buffer){0};
    df_error(out, "closed identity");
    return 0;
  }
  static const char digits[] = "0123456789abcdef";
  char hex[65];
  for (unsigned i = 0; i < 32; ++i) {
    hex[2 * i] = digits[key[i] >> 4];
    hex[2 * i + 1] = digits[key[i] & 15];
  }
  hex[64] = 0;
  return attach(n, store, identity, hex, NULL, NULL, out);
}
uint32_t df_network_subscribe(void *n, void *store, const char *key, df_publication cb, void *ctx,
                              df_buffer *out) {
  return attach(n, store, NULL, key, cb, ctx, out);
}
uint32_t df_network_follow(df_network *n, df_store *s, df_buffer *out) {
  return attach(n, s, NULL, s ? s->scope->name : NULL, NULL, NULL, out);
}
df_status df_network_detach(df_network *n, uint32_t id, df_buffer *out) {
  *out = (df_buffer){0};
  if (!n || n->pumping) {
    df_error(out, "detach must run outside pump and callbacks");
    return DF_ERROR;
  }
  ct_pair *entry = ct_map_get(&n->channel_index, id);
  if (!entry)
    return DF_ABSENT;
  size_t at = entry->value;
  channel *c = n->channels + at;
  if (n->fd >= 0 && !send_request(n, id, c, 5, df_span(NULL, 0), NULL)) {
    df_error(out, n->error);
    return DF_ERROR;
  }
  df_sync_cancel(c->store);
  c->store->network_owner = NULL;
  df_writer_free(&c->opening);
  ct_map_erase(&n->channel_index, id);
  --n->count;
  if (at != n->count) {
    *c = n->channels[n->count];
    ct_map_get(&n->channel_index, c->id)->value = at;
  }
  if (n->next_channel >= n->count)
    n->next_channel = 0;
  if (!n->count)
    reset(n);
  return DF_OK;
}
void df_network_pump(void *p, uint64_t budget) {
  network *n = p;
  if (!n || n->pumping)
    return;
  n->pumping = true;
  if (!pump(n, budget))
    fail(n);
  n->pumping = false;
}
static void watch_deadline(df_network_watch *w, uint64_t now, uint64_t base, uint64_t span) {
  uint64_t end = span > UINT64_MAX - base ? UINT64_MAX : base + span;
  uint64_t delay = end > now ? end - now : 0;
  if (delay < w->timeout_us)
    w->timeout_us = delay;
}
df_network_watch df_network_prepare(df_network *n) {
  df_network_watch w = {-1, 0, UINT64_MAX};
  if (!n || !n->enabled || !n->count)
    return w;
  uint64_t now = now_us();
  if (n->fd < 0) {
    watch_deadline(&w, now, n->retry, 0);
    return w;
  }
  w.fd = n->fd;
  w.events = DF_NETWORK_READ;
  if (n->connecting || n->head)
    w.events |= DF_NETWORK_WRITE;
  if (n->connecting) {
    watch_deadline(&w, now, n->connected_at, 10000001);
    return w;
  }
  /* A budget-limited pump can leave complete messages in the user buffer even
   * after the kernel socket has drained. Do not wait for another read event. */
  if (n->input.size >= 4) {
    df_reader r = {df_written(&n->input), false};
    uint32_t size = df_read32(&r);
    if (size < 25 || size > FRAME_LIMIT || n->input.size - 4 >= size)
      w.timeout_us = 0;
  }
  for (size_t i = 0; i < n->count; ++i) {
    channel *c = n->channels + i;
    if (c->request) {
      watch_deadline(&w, now, c->sent, 10000001);
      watch_deadline(&w, now, c->started, 30000001);
    } else if (!c->signer && c->opening.size) {
      w.timeout_us = 0;
    } else if (c->state == IDLE) {
      bool pending = c->signer
                         ? c->store->revision != c->pushed && n->queued <= QUEUE_LIMIT - 512 * 1024
                         : !c->subscribed;
      if (pending)
        w.timeout_us = 0;
    }
  }
  if (n->heartbeat)
    watch_deadline(&w, now, n->heartbeat_sent, 10000001);
  else
    watch_deadline(&w, now, n->last_ping,
                   n->heartbeat_interval == UINT64_MAX ? UINT64_MAX : n->heartbeat_interval + 1);
  return w;
}
int df_network_measure(void *p, uint64_t interval, df_buffer *out) {
  network *n = p;
  *out = (df_buffer){0};
  if (!n || interval > UINT64_MAX / 1000) {
    df_error(out, "invalid heartbeat interval");
    return 1;
  }
  if (!n->samples) {
    n->samples = calloc(4096, sizeof(*n->samples));
    if (!n->samples) {
      df_error(out, "out of memory");
      return 1;
    }
  }
  n->measuring = true;
  n->heartbeat_interval = interval ? interval * 1000 : 10000000;
  return 0;
}
void df_network_online(void *p, int enabled) {
  network *n = p;
  if (n->enabled == (enabled != 0))
    return;
  n->enabled = enabled != 0;
  reset(n);
  n->retry = 0;
}
int df_network_connected(void *p) {
  network *n = p;
  return n->fd >= 0 && !n->connecting;
}
int df_network_settled(void *p) {
  network *n = p;
  if (!df_network_connected(n) || n->queued)
    return 0;
  for (size_t i = 0; i < n->count; ++i) {
    channel *c = n->channels + i;
    if (c->request || c->opening.size || c->state != IDLE ||
        (c->signer ? c->pushed != c->store->revision : !c->subscribed))
      return 0;
  }
  return 1;
}
const char *df_network_error(void *p) { return ((network *)p)->error; }
const df_network_stats *df_network_statistics(void *p) { return &((network *)p)->stats; }
const df_request_timing *df_network_samples(void *p, size_t *size) {
  network *n = p;
  *size = n->sample_count;
  return n->samples;
}
