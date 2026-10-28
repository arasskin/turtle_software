/* Linux transport: one io_uring completion loop for sockets and file storage. */
#define _GNU_SOURCE
#include "../containers/map.h"
#include "../datafriend/schema.h"
#include "auth.h"
#include "storage_uring.h"
#include "sync.h"
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <sys/random.h>
#include <time.h>
#include "connections.h"
/* Bounded per-batch share of the global storage pool. Override to 1 only when
 * building the serial benchmark control; this is not a wire/runtime option. */
#ifndef LB_WRITE_WINDOW
#define LB_WRITE_WINDOW 16
#endif
typedef struct loop {
  struct io_uring ring;
  int listener;
  bool accepting, cancelling_accept, accept_cancel_requested;
  completion accept, cancel_accept;
  connections connections;
  uint64_t accept_retry;
  bool shutdown_started;
} loop;
typedef struct options {
  const char *values, *bind;
  int port;
  size_t memory_budget;
} options;
static volatile sig_atomic_t stopping;
static void stop(int sig) {
  (void)sig;
  stopping = 1;
}
static uint64_t now_ms(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t))
    abort();
  return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
static void reset(loop *s, client *c) {
  if (c->receiving || c->sending || c->file_pending || c->cancelling_receive || c->cancelling_send)
    abort();
  storage_wait_remove(&s->connections, c);
  lb_task_dispose(&c->task);
  for (size_t i = 0; i < c->channel_count; ++i)
    subscription_remove(&s->connections, c->channels[i].subscription);
  output_update_release(&s->connections, c);
  buffer_return(&s->connections.buffers, &c->input);
  buffer_return(&s->connections.buffers, &c->output);
  free(c->channels);
  free(c->extra_files);
  ct_map_free(&c->channel_index);
  if (c->fd >= 0) close(c->fd);
  connection_release(&s->connections, c);
}
/* SQ exhaustion is a scheduling condition, not a connection error. These
 * connections resume after submissions are flushed, even without a CQE. */
static struct io_uring_sqe *client_sqe(loop *s, client *c) {
  struct io_uring_sqe *sqe = io_uring_get_sqe(&s->ring);
  if (!sqe) queue_push(&s->connections.submission_wait, c);
  return sqe;
}
static void submit_accept(loop *s) {
  struct io_uring_sqe *sqe;
  if (s->accepting || stopping || now_ms() < s->accept_retry)
    return;
  sqe = io_uring_get_sqe(&s->ring);
  if (!sqe)
    return;
  io_uring_prep_accept(sqe, s->listener, NULL, NULL, SOCK_CLOEXEC);
  io_uring_sqe_set_data(sqe, &s->accept);
  s->accepting = true;
}
static void cancel_socket(loop *s, client *c, bool pending, bool *requested, bool *cancelling,
                          completion *original, completion *tag) {
  struct io_uring_sqe *sqe;
  if (!pending || *requested)
    return;
  sqe = client_sqe(s, c);
  if (!sqe)
    return;
  io_uring_prep_cancel(sqe, original, 0);
  io_uring_sqe_set_data(sqe, tag);
  *requested = *cancelling = true;
}
static void close_client(loop *s, client *c) {
  c->closing = true;
  storage_wait_remove(&s->connections, c);
  timer_remove(&s->connections, c);
  cancel_socket(s, c, c->receiving, &c->cancelled_receive, &c->cancelling_receive, &c->receive,
                &c->cancel_receive);
  cancel_socket(s, c, c->sending, &c->cancelled_send, &c->cancelling_send, &c->send, &c->cancel_send);
  if (!c->file_pending) {
    lb_task_dispose(&c->task);
    c->task_active = false;
  }
  if (!c->receiving && !c->sending && !c->file_pending && !c->cancelling_receive &&
      !c->cancelling_send)
    reset(s, c);
}
static bool frame(loop *s, client *c, uint32_t request, uint32_t id, lb_bytes reply) {
  if (reply.size + 8 > LB_MAX_FRAME)
    return false;
  assert(!c->output.data && !c->output_update);
  if (!buffer_borrow(&s->connections, &c->output, reply.size + 12)) return false;
  lb_u32(&c->output, (uint32_t)reply.size + 8);
  lb_u32(&c->output, request);
  lb_u32(&c->output, id);
  lb_append(&c->output, reply.data, reply.size);
  c->sent = 0;
  return !c->output.failed;
}
static void dispatch(loop *s, client *c, lb_service *service) {
  lb_request request;
  lb_buffer reply = CT_ZERO;
  uint8_t name[16];
  lb_auth *proof;
  ct_pair *found;
  bool ready = true;
  uint8_t status;
  c->active = SIZE_MAX;
  c->request_id = c->channel_id = 0;
  c->op = 255;
  c->task_active = true;
  if (c->input.size >= 12) {
    lb_reader r = {lb_bytes_make(c->input.data + 4, c->input.size - 4), false};
    c->request_id = lb_read_u32(&r);
    c->channel_id = lb_read_u32(&r);
  }
  if (!lb_parse(lb_buffer_bytes(&c->input),
                lb_bytes_make(DATAFRIEND_SCHEMA, sizeof(DATAFRIEND_SCHEMA) - 1), &request))
    goto reject;
  c->op = request.op;
  if (request.op == 7) {
    lb_reply_header(&reply, 0, 0, 0);
    goto done;
  }
  if (!lb_auth_namespace(request.public_key, name))
    goto reject;
  found = ct_map_get(&c->channel_index, request.channel);
  if (found)
    c->active = (size_t)found->value;
  else {
    if (!ct_array_reserve_limit(&c->channels, &c->channel_capacity, c->channel_count + 1,
                                sizeof(*c->channels), UINT32_MAX) ||
        !ct_map_put(&c->channel_index, request.channel, c->channel_count))
      goto reject;
    c->active = c->channel_count++;
    memset(&c->channels[c->active], 0, sizeof(*c->channels));
    c->channels[c->active].id = request.channel;
    memcpy(c->channels[c->active].name, name, 16);
  }
  if (memcmp(c->channels[c->active].name, name, 16))
    goto reject;
  proof = &c->channels[c->active].auth;
  if ((request.op == 3 || request.op == 6) && !lb_auth_verify(proof, &request))
    goto reject;
  if (request.op == 6 && c->channels[c->active].needs_reconcile) {
    /* Authenticated uploads remain silent while reconciliation is required. */
    lb_task_ready(&c->task, &reply);
    c->task.no_ack = c->task.succeeded = true;
    ready = false;
  } else if (request.op == 4 || request.op == 8) {
    if (!lb_opening(service, lb_name(name), 0, &reply))
      goto reject;
  } else if (request.op == 5)
    lb_reply_header(&reply, 0, request.epoch, request.revision);
  else if (request.op <= 1) {
    if (!lb_handle(service, request.op, lb_name(name), false, request.epoch, request.revision,
                   request.payload, &reply) ||
        !reply.size)
      goto reject;
    status = reply.data[0];
    if (request.op == 0 && (status == 0 || status == 3)) {
      c->channels[c->active].needs_reconcile = false;
      memset(proof, 0, sizeof(*proof));
      proof->pushing = request.payload.data[0] == 1;
      if (proof->pushing && status == 3 && !lb_auth_challenge(proof, &reply))
        goto reject;
    } else if (request.op == 1 && status == 0 && proof->pushing && !proof->issued &&
               !lb_exchange_equal(request.payload,
                                  lb_bytes_make(reply.data + 17, reply.size - 17))) {
      if (!lb_auth_challenge(proof, &reply))
        goto reject;
    }
  } else {
    lb_task_init(&c->task, service, request.op, lb_name(name), request.op == 3 || request.op == 6,
                 request.epoch, request.revision, request.payload);
    c->task.write_limit = LB_WRITE_WINDOW;
    c->task.no_ack = request.op == 6;
    ready = false;
  }
  goto done;
reject:
  lb_buffer_free(&reply);
  lb_task_reject(&c->task);
  ready = false;
done:
  if (ready)
    lb_task_ready(&c->task, &reply);
  lb_buffer_free(&reply);
  buffer_return(&s->connections.buffers, &c->input);
  c->received = 0;
  c->body = false;
  connection_account(&s->connections, c);
  if (!connection_budget(&s->connections)) c->closing = true;
}
static bool finish(loop *s, client *c, lb_buffer *reply) {
  uint8_t status;
  channel *ch = c->active == SIZE_MAX ? NULL : &c->channels[c->active];
  if (c->op == 6 && c->task.no_ack && c->task.succeeded) {
    c->active = SIZE_MAX;
    return true;
  }
  if (reply->failed || reply->size < 17)
    return false;
  status = reply->data[0] & 0x7f;
  if (ch) {
    if ((c->op == 4 || c->op == 8) && status != 2) {
      ch->subscribed = true;
      ch->optimistic = c->op == 8;
      if (!subscription_add(&s->connections, c, ch)) return false;
    }
    if (ch->subscribed &&
        (status == 3 || ((c->op == 4 || c->op == 8 || status == 1) && reply->size > 17))) {
      ch->announced = lb_integer(lb_bytes_make(reply->data + 9, 8));
      ch->known = true;
      /* This reply opens the current state, superseding unsent older deltas. */
      if (ch->subscription) {
        deliveries_clear(&s->connections, ch->subscription);
        ch->subscription->resync = false;
      }
    }
    if (c->op == 5 && status == 0) {
      subscription_remove(&s->connections, ch->subscription);
      ct_map_erase(&c->channel_index, ch->id);
      --c->channel_count;
      if (c->active != c->channel_count) {
        *ch = c->channels[c->channel_count];
        ct_map_get(&c->channel_index, ch->id)->value = c->active;
      }
    }
  }
  if (c->op == 6) {
    if (status == 1) {
      if (ch)
        ch->needs_reconcile = true;
      reply->size = 17;
      reply->data[0] = 6;
      if (!frame(s, c, 0, c->channel_id, lb_buffer_bytes(reply)))
        return false;
    } else if (status != 0 && !frame(s, c, 0, c->channel_id, lb_buffer_bytes(reply)))
      return false;
  } else if (!frame(s, c, c->request_id, c->channel_id, lb_buffer_bytes(reply)))
    return false;
  c->active = SIZE_MAX;
  return true;
}
/* Consume at most one dirty subscription per turn, including unchanged or
 * busy entries. A writer's unlock notification requeues busy subscriptions. */
static bool event(loop *s, client *c, lb_service *service) {
  subscription *sub = c->dirty_head;
  channel *ch;
  lb_namespace ns;
  uint64_t revision;
  lb_buffer reply = CT_ZERO;
  ct_pair *found;
  bool ok;
  if (!sub) return true;
  found = ct_map_get(&c->channel_index, sub->channel_id);
  assert(found);
  ch = &c->channels[found->value];
  subscription_clean(sub);
  if (sub->head && !sub->resync) {
    assert(ch->known && ch->announced == sub->head->update->before);
    delivery_start(&s->connections, sub, ch);
    /* Rotate subscriptions, but keep the remainder of this chain runnable. */
    subscription_dirty(&s->connections, sub);
    return true;
  }
  ns = lb_name(ch->name);
  if (lb_service_writing(service, ns)) return true;
  revision = lb_service_revision(service, ns);
  if (ch->known && ch->announced == revision && !sub->resync) return true;
  ok = lb_opening(service, ns, 4, &reply);
  if (ok) ok = frame(s, c, 0, ch->id, lb_buffer_bytes(&reply));
  lb_buffer_free(&reply);
  if (!ok) return false;
  ch->known = true;
  ch->announced = revision;
  sub->resync = false;
  return true;
}
static void advance(loop *s, client *c, lb_service *service) {
  struct io_uring_sqe *sqe;
  size_t length;
  void *buffer;
  if (c->fd < 0)
    return;
  if (stopping || now_ms() > c->deadline)
    c->closing = true;
  if (c->closing) {
    close_client(s, c);
    return;
  }
  if (!c->sending && !client_output_size(c) && !c->task_active && c->body && c->received == c->input.size)
    dispatch(s, c, service);
  if (c->closing) {
    close_client(s, c);
    return;
  }
  if (c->task_active) {
    if (c->task.abort_status) {
      storage_wait_remove(&s->connections, c);
      lb_task_step_budget(&c->task, 0);
    }
    /* Submit already-reserved requests even while parked for more capacity.
     * This also resubmits partial/EAGAIN writes without needing a free slot. */
    if (lb_task_io_ready(&c->task)) {
      if (c->file_capacity < c->task.extra_count) {
        completion *tags;
        if (c->file_pending) abort();
        tags = realloc(c->extra_files, c->task.extra_count * sizeof(*tags));
        if (!tags) { close_client(s, c); return; }
        c->extra_files = tags;
        c->file_capacity = c->task.extra_count;
        connection_account(&s->connections, c);
        if (!connection_budget(&s->connections)) { close_client(s, c); return; }
      }
      for (size_t slot = 0; slot <= c->task.extra_count; ++slot) {
        lb_store_request *io = lb_task_io(&c->task, slot);
        completion *tag = slot ? &c->extra_files[slot - 1] : &c->file;
        if (!io->owner || io->state != lb_phase_ready) continue;
        *tag = (completion){op_file, c, io};
        if (!lb_uring_enqueue(&s->ring, io, tag)) {
          queue_push(&s->connections.submission_wait, c);
          break;
        }
        ++c->file_pending;
      }
      return;
    }
    if (c->storage_queued || (!c->task.extra_count && c->file_pending)) return;
    if (c->task.phase != lb_task_done)
      lb_task_step_budget(&c->task, 64);
    connection_account(&s->connections, c);
    if (!connection_budget(&s->connections)) {
      close_client(s, c);
      return;
    }
    if (c->task.waiting_capacity) {
      storage_wait_push(&s->connections, c);
    }
    if (c->task.phase == lb_task_done) {
      if (c->file_pending) abort();
      storage_wait_remove(&s->connections, c);
      bool ok = finish(s, c, &c->task.reply);
      lb_task_dispose(&c->task);
      c->task_active = false;
      if (!ok) {
        close_client(s, c);
        return;
      }
    }
  }
  if (c->task_active) {
    if (lb_task_io_ready(&c->task) || (!c->file_pending && !c->storage_queued))
      queue_push(&s->connections.ready, c);
    return;
  }
  if (!c->sending && !client_output_size(c) && !event(s, c, service)) {
    close_client(s, c);
    return;
  }
  if (client_output_size(c) && !c->sending) {
    sqe = client_sqe(s, c);
    if (sqe) {
      if (c->output_update) {
        /* Both vectors and their storage stay pinned until the send CQE. */
        size_t offset = c->sent, count = 0;
        memset(&c->output_message, 0, sizeof(c->output_message));
        if (offset < sizeof(c->output_header)) {
          c->output_vectors[count++] = (struct iovec){c->output_header + offset,
                                                      sizeof(c->output_header) - offset};
          offset = 0;
        } else offset -= sizeof(c->output_header);
        c->output_vectors[count++] = (struct iovec){c->output_update->data + offset,
                                                    c->output_update->size - offset};
        c->output_message.msg_iov = c->output_vectors;
        c->output_message.msg_iovlen = count;
        io_uring_prep_sendmsg(sqe, c->fd, &c->output_message, MSG_NOSIGNAL);
      } else
        io_uring_prep_send(sqe, c->fd, c->output.data + c->sent, c->output.size - c->sent,
                           MSG_NOSIGNAL);
      io_uring_sqe_set_data(sqe, &c->send);
      c->sending = true;
    }
  }
  length = c->body ? c->input.size : sizeof(c->header);
  if (!c->task_active && !client_output_size(c) && !c->receiving && c->received < length) {
    sqe = client_sqe(s, c);
    if (sqe) {
      buffer = c->body ? (void *)(c->input.data + c->received) : (void *)(c->header + c->received);
      io_uring_prep_recv(sqe, c->fd, buffer, length - c->received, 0);
      io_uring_sqe_set_data(sqe, &c->receive);
      c->receiving = true;
    }
  }
  if (!client_output_size(c) && c->dirty_head && !c->queue)
    queue_push(&s->connections.ready, c);
}
static void complete(loop *s, completion *tag, int result) {
  client *c;
  size_t i;
  if (tag->kind == op_accept) {
    int no_delay = 1;
    s->accepting = false;
    if (result < 0) {
      if (!stopping) s->accept_retry = now_ms() + 100;
      return;
    }
    if (stopping || setsockopt(result, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay))) {
      close(result);
      return;
    }
    c = connection_budget(&s->connections) ? connection_take(&s->connections) : NULL;
    if (!c) {
      close(result);
      s->accept_retry = now_ms() + 100;
      return;
    }
    c->fd = result;
    if (!connection_budget(&s->connections)) {
      reset(s, c);
      s->accept_retry = now_ms() + 100;
      return;
    }
    timer_touch(&s->connections, c, now_ms() + 30000);
    queue_push(&s->connections.ready, c);
    return;
  }
  if (tag->kind == op_cancel_accept) {
    s->cancelling_accept = false;
    return;
  }
  c = tag->owner;
  queue_push(&s->connections.ready, c);
  if (tag->kind == op_cancel_receive) {
    c->cancelling_receive = false;
    return;
  }
  if (tag->kind == op_cancel_send) {
    c->cancelling_send = false;
    return;
  }
  if (tag->kind == op_file) {
    lb_store_result rc;
    if (!c->file_pending) abort();
    --c->file_pending;
    rc = lb_store_complete_io(tag->io, result);
    if (rc.code != lb_error_ok && rc.code != lb_error_more_io)
      lb_task_reject(&c->task);
    return;
  }
  if (tag->kind == op_receive)
    c->receiving = false;
  else
    c->sending = false;
  if (c->closing)
    return;
  if (result == -EINTR || result == -EAGAIN)
    return;
  if (result <= 0) {
    c->closing = true;
    return;
  }
  timer_touch(&s->connections, c, now_ms() + 30000);
  if (tag->kind == op_send) {
    c->sent += (size_t)result;
    if (c->sent == client_output_size(c)) {
      if (c->output_update) output_update_release(&s->connections, c);
      else buffer_return(&s->connections.buffers, &c->output);
      c->sent = 0;
    }
    return;
  }
  c->received += (size_t)result;
  if (!c->body && c->received == sizeof(c->header)) {
    uint32_t length = 0;
    for (i = 0; i < sizeof(c->header); ++i)
      length = (length << 8) | c->header[i];
    if (length < 13 || length > LB_MAX_FRAME || !buffer_borrow(&s->connections, &c->input, length)) {
      c->closing = true;
      return;
    }
    c->input.size = length;
    c->received = 0;
    c->body = true;
  }
}
static void run(loop *s, lb_service *service) {
  connections *cs = &s->connections;
  for (;;) {
    int submitted, waited;
    struct io_uring_cqe *cqe = NULL;
    struct __kernel_timespec timeout;
    uint64_t now, delay;
    /* Always service CQEs between bounded batches of runnable work. */
    while (!io_uring_peek_cqe(&s->ring, &cqe)) {
      completion *tag = (completion *)io_uring_cqe_get_data(cqe);
      int result = cqe->res;
      io_uring_cqe_seen(&s->ring, cqe);
      if (tag) complete(s, tag, result);
    }
    if (stopping && !s->shutdown_started) {
      s->shutdown_started = true;
      for (client *c = cs->live; c; c = c->live_next) {
        c->closing = true;
        queue_push(&cs->ready, c);
      }
    }
    now = now_ms();
    while (cs->timer_count && cs->timers[0]->deadline <= now) {
      client *c = cs->timers[0];
      timer_remove(cs, c);
      c->closing = true;
      queue_push(&cs->ready, c);
    }
    if (stopping && s->accepting && !s->accept_cancel_requested) {
      struct io_uring_sqe *sqe = io_uring_get_sqe(&s->ring);
      if (sqe) {
        io_uring_prep_cancel(sqe, &s->accept, 0);
        io_uring_sqe_set_data(sqe, &s->cancel_accept);
        s->cancelling_accept = s->accept_cancel_requested = true;
      }
    }
    submit_accept(s);
    for (size_t turns = 0; turns < 256; ++turns) {
      client *c = connection_next(cs, &service->cache);
      if (!c) break;
      advance(s, c, service);
      if (c->fd >= 0) {
        connection_account(cs, c);
        if (!connection_budget(cs)) {
          c->closing = true;
          queue_push(&cs->ready, c);
        }
      }
    }
    if (stopping && !cs->count && !s->accepting && !s->cancelling_accept) return;
    submitted = io_uring_submit(&s->ring);
    if (submitted < 0 && submitted != -EINTR) {
      fprintf(stderr, "io_uring submission failed: %d\n", submitted);
      _Exit(2);
    }
    /* Submission capacity does not depend on an operation completing. */
    if (submitted >= 0) {
      client *c;
      while ((c = queue_pop(&cs->submission_wait))) queue_push(&cs->ready, c);
    }
    if (cs->ready.head || submitted == -EINTR ||
        (cs->storage_head && service->cache.pending_count < service->cache.pending_limit)) continue;
    now = now_ms();
    /* A bounded sleep also services POSIX shutdown signals if SA_RESTART is
     * enabled. No live-client scan is performed on this periodic wakeup. */
    delay = 1000;
    if (cs->timer_count) {
      uint64_t due = cs->timers[0]->deadline;
      if (due <= now) delay = 0;
      else if (due - now < delay) delay = due - now;
    }
    if (!s->accepting && !stopping) {
      uint64_t retry = s->accept_retry > now ? s->accept_retry - now : 0;
      if (retry < delay) delay = retry;
    }
    timeout.tv_sec = (long long)(delay / 1000);
    timeout.tv_nsec = (long long)(delay % 1000) * 1000000;
    waited = io_uring_wait_cqe_timeout(&s->ring, &cqe, &timeout);
    if (waited < 0 && waited != -ETIME && waited != -EINTR) {
      fprintf(stderr, "io_uring wait failed: %d\n", waited);
      _Exit(2);
    }
  }
}
static bool parse_options(int argc, char **argv, options *out) {
  int i;
  out->values = NULL;
  out->bind = "127.0.0.1";
  out->port = 7778;
  out->memory_budget = 256 * 1024 * 1024;
  for (i = 1; i < argc; i += 2) {
    if (i + 1 == argc)
      return false;
    if (!strcmp(argv[i], "--values"))
      out->values = argv[i + 1];
    else if (!strcmp(argv[i], "--bind"))
      out->bind = argv[i + 1];
    else if (!strcmp(argv[i], "--port")) {
      char *end = NULL;
      long port;
      errno = 0;
      port = strtol(argv[i + 1], &end, 10);
      if (errno || *end || end == argv[i + 1] || port < 0 || port > 65535)
        return false;
      out->port = (int)port;
    } else if (!strcmp(argv[i], "--memory-budget")) {
      char *end = NULL;
      unsigned long long bytes;
      errno = 0;
      bytes = strtoull(argv[i + 1], &end, 10);
      if (errno || *end || end == argv[i + 1] || argv[i + 1][0] == '-' ||
          bytes < sizeof(client_slab) || bytes > SIZE_MAX) return false;
      out->memory_budget = (size_t)bytes;
    } else
      return false;
  }
  return out->values != NULL;
}
static bool boot_nonce(uint64_t *nonce) {
  size_t done = 0;
  while (done < sizeof(*nonce)) {
    ssize_t n = getrandom((char *)nonce + done, sizeof(*nonce) - done, 0);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return false;
    done += (size_t)n;
  }
  return true;
}
int main(int argc, char **argv) {
  options args;
  lb_service service = CT_ZERO;
  loop s = CT_ZERO;
  struct io_uring_params params = CT_ZERO;
  struct sockaddr_in address = CT_ZERO;
  socklen_t size = sizeof(address);
  uint64_t nonce;
  int reuse = 1, result = 1;
  bool ring_open = false, service_open = false;
  s.listener = -1;
  s.accept.kind = op_accept;
  s.cancel_accept.kind = op_cancel_accept;
  if (!parse_options(argc, argv, &args)) {
    fprintf(stderr, "expected --values path [--bind address] [--port number] [--memory-budget bytes]\n");
    return 1;
  }
  s.connections.memory_budget = args.memory_budget;
  if (sodium_init() < 0 || !boot_nonce(&nonce)) {
    fprintf(stderr, "cryptography initialization failed\n");
    goto done;
  }
  if (lb_service_create(&service, args.values, nonce, UINT64_C(1) << 30).code != lb_error_ok) {
    fprintf(stderr, "cache creation failed\n");
    goto done;
  }
  service_open = true;
  service.changed = connections_changed;
  service.changed_context = &s.connections;
  params.flags =
      IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_DEFER_TASKRUN;
  if (io_uring_queue_init_params(64, &s.ring, &params) < 0) {
    fprintf(stderr, "io_uring setup failed\n");
    goto done;
  }
  ring_open = true;
  s.listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (s.listener < 0) {
    perror("socket");
    goto done;
  }
  setsockopt(s.listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  address.sin_family = AF_INET;
  address.sin_port = htons((uint16_t)args.port);
  if (inet_pton(AF_INET, args.bind, &address.sin_addr) != 1 ||
      bind(s.listener, (struct sockaddr *)&address, sizeof(address)) || listen(s.listener, SOMAXCONN)) {
    perror("listen");
    goto done;
  }
  getsockname(s.listener, (struct sockaddr *)&address, &size);
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, stop);
  signal(SIGINT, stop);
  printf("Littlebear TCP listening %s:%u\n", args.bind, (unsigned)ntohs(address.sin_port));
  fflush(stdout);
  run(&s, &service);
  result = 0;
done:
  if (s.listener >= 0)
    close(s.listener);
  if (ring_open)
    io_uring_queue_exit(&s.ring);
  if (service_open)
    lb_service_destroy(&service);
  connections_destroy(&s.connections);
  return result;
}
