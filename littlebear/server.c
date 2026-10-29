/* Linux transport: one io_uring completion loop for sockets and file storage. */
#define _GNU_SOURCE
#include "../containers/map.h"
#include "../datafriend/schema.h"
#include "storage_uring.h"
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <sys/random.h>
#include <time.h>
#include "client_connections.h"
#include "websocket.h"
#include "models.h"
#include "codec.h"
#include "namespace_channels.h"
#include "owner_authorization.h"
#include "subscriber_delivery.h"
#include "op_0_begin_owner_sync.h"
#include "op_1_continue_reconciliation.h"
#include "op_2_fetch_records.h"
#include "op_3_apply_records_at_revision.h"
#include "op_5_close_channel.h"
#include "op_6_stream_record_changes.h"
#include "op_7_heartbeat.h"
#include "op_8_subscribe_to_records.h"
/* Server is the only network dispatcher. Operations own authorization, policy,
 * resumable work and completion; transports only deliver their envelopes. */
/* Bounded per-batch share of the global storage pool. Override to 1 only when
 * building the serial benchmark control; this is not a wire/runtime option. */
#ifndef LB_WRITE_WINDOW
#define LB_WRITE_WINDOW 16
#endif
typedef struct listener {
  int fd;
  bool accepting, cancelling_accept, accept_cancel_requested;
  completion accept, cancel_accept;
  uint64_t accept_retry;
} listener;
typedef struct loop {
  struct io_uring ring;
  listener listeners[2]; /* TCP, optional WebSocket */
  connections connections;
  bool shutdown_started;
} loop;
typedef struct options {
  const char *values, *bind;
  int port, websocket_port;
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
static void publish_changes(lb_task *t, lb_subscribers *subscribers) {
  if (!t->changed) return;
  subscribers_changed(subscribers, t->ns, t->publish ? &t->publication : NULL);
  t->changed = false;
}
static void dispose_task(lb_task *t, lb_directory *namespaces, size_t *active_writers,
                         lb_subscribers *subscribers) {
  /* Socket close cannot release buffers while the kernel still owns file I/O. */
  if (!lb_task_drain(t)) abort();
  switch (t->operation) {
  case lb_op_apply_records_at_revision: lb_apply_records_at_revision_release(t, namespaces, active_writers); break;
  case lb_op_stream_record_changes: lb_stream_record_changes_release(t, namespaces, active_writers); break;
  default: break;
  }
  publish_changes(t, subscribers);
  switch (t->operation) {
  case lb_op_fetch_records: lb_fetch_records_dispose(t); break;
  case lb_op_apply_records_at_revision: lb_apply_records_at_revision_dispose(t); break;
  case lb_op_stream_record_changes: lb_stream_record_changes_dispose(t); break;
  default: lb_task_dispose(t); break;
  }
}
static void resume_task(lb_task *t, lb_directory *namespaces, uint64_t epoch,
                        lb_store *records, size_t *active_writers, size_t budget) {
  switch (t->operation) {
  case lb_op_begin_owner_sync:
    lb_begin_owner_sync_resume(t, namespaces, epoch, budget); break;
  case lb_op_continue_reconciliation:
    lb_continue_reconciliation_resume(t, namespaces, epoch, budget); break;
  case lb_op_fetch_records:
    lb_fetch_records_resume(t, namespaces, epoch, records, budget); break;
  case lb_op_apply_records_at_revision:
    lb_apply_records_at_revision_resume(t, namespaces, epoch, records, active_writers, budget); break;
  case lb_op_stream_record_changes:
    lb_stream_record_changes_resume(t, namespaces, epoch, records, active_writers, budget); break;
  default: break;
  }
}
static void reset(loop *s, client *c, lb_directory *namespaces, size_t *active_writers) {
  if (c->receiving || c->sending || c->file_pending || c->cancelling_receive || c->cancelling_send)
    abort();
  storage_wait_remove(&s->connections, c);
  dispose_task(&c->task, namespaces, active_writers, s->connections.subscribers);
  for (size_t i = 0; i < c->peer.channel_count; ++i)
    subscription_remove(s->connections.subscribers, c->peer.channels[i].subscription);
  output_update_release(s->connections.subscribers, &c->peer);
  buffer_return(s->connections.buffers, &c->input);
  buffer_return(s->connections.buffers, &c->output);
  free(c->peer.channels);
  free(c->extra_files);
  free(c->websocket);
  ct_map_free(&c->peer.channel_index);
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
static void submit_accept(loop *s, listener *l) {
  struct io_uring_sqe *sqe;
  if (l->fd < 0 || l->accepting || stopping || now_ms() < l->accept_retry) return;
  sqe = io_uring_get_sqe(&s->ring);
  if (!sqe) return;
  io_uring_prep_accept(sqe, l->fd, NULL, NULL, SOCK_CLOEXEC);
  io_uring_sqe_set_data(sqe, &l->accept);
  l->accepting = true;
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
static void close_client(loop *s, client *c, lb_directory *namespaces, size_t *active_writers) {
  c->closing = c->peer.closing = true;
  storage_wait_remove(&s->connections, c);
  timer_remove(&s->connections, c);
  cancel_socket(s, c, c->receiving, &c->cancelled_receive, &c->cancelling_receive, &c->receive,
                &c->cancel_receive);
  cancel_socket(s, c, c->sending, &c->cancelled_send, &c->cancelling_send, &c->send, &c->cancel_send);
  if (!c->file_pending) {
    dispose_task(&c->task, namespaces, active_writers, s->connections.subscribers);
    c->task_active = false;
  }
  if (!c->receiving && !c->sending && !c->file_pending && !c->cancelling_receive &&
      !c->cancelling_send)
    reset(s, c, namespaces, active_writers);
}
static bool frame(loop *s, client *c, uint32_t request, uint32_t id, lb_bytes reply) {
  if (reply.size + 8 > LB_MAX_FRAME)
    return false;
  assert(!c->output.data && !c->peer.output_update);
  if (!buffer_borrow(&s->connections, &c->output, reply.size + 12)) return false;
  lb_u32(&c->output, (uint32_t)reply.size + 8);
  lb_u32(&c->output, request);
  lb_u32(&c->output, id);
  lb_append(&c->output, reply.data, reply.size);
  c->sent = 0;
  return !c->output.failed;
}
static bool parse_request_envelope(lb_bytes packet, lb_bytes schema, lb_request *out) {
  lb_reader r = {packet, false};
  memset(out, 0, sizeof(*out));
  if (!lb_bytes_equal(lb_take(&r, sizeof(lb_envelope_magic)),
                      lb_bytes_make(lb_envelope_magic, sizeof(lb_envelope_magic))))
    return false;
  out->id = lb_read_u32(&r);
  out->channel = lb_read_u32(&r);
  out->op = lb_read_u8(&r);
  if (!out->id || !lb_operation_supported(out->op))
    return false;
  out->public_key = lb_read_blob(&r);
  if (!lb_bytes_equal(lb_read_blob(&r), schema))
    return false;
  if (lb_operation_connection_scope(out->op)) {
    if (out->channel || out->public_key.size)
      return false;
  } else if (!out->channel || out->public_key.size != LB_PUBLIC_KEY_BYTES)
    return false;
  out->epoch = lb_read_u64(&r);
  out->revision = lb_read_u64(&r);
  out->payload = lb_read_blob(&r);
  if (lb_operation_signed(out->op)) {
    out->challenge = lb_take(&r, LB_CHALLENGE_BYTES);
    out->sequence = lb_read_u64(&r);
    out->signature = lb_take(&r, LB_SIGNATURE_BYTES);
    if (packet.size < LB_SIGNATURE_BYTES)
      return false;
    out->signed_bytes = lb_bytes_make(packet.data, packet.size - LB_SIGNATURE_BYTES);
  }
  if (!lb_reader_end(&r))
    return false;
  if (lb_operation_empty_payload(out->op) && out->payload.size)
    return false;
  return true;
}
static void dispatch(loop *s, client *c, lb_directory *namespaces, uint64_t epoch) {
  lb_request request;
  channel *ch = NULL;
  bool begun = false;
  c->peer.active = SIZE_MAX;
  c->request_id = c->channel_id = 0;
  c->task.operation = UINT_MAX;
  c->task_active = true;
  if (c->input.size >= 12) {
    lb_reader reader = {lb_bytes_make(c->input.data + 4, c->input.size - 4), false};
    c->request_id = lb_read_u32(&reader);
    c->channel_id = lb_read_u32(&reader);
  }
  if (!parse_request_envelope(lb_buffer_bytes(&c->input),
                lb_bytes_make(DATAFRIEND_SCHEMA, sizeof(DATAFRIEND_SCHEMA) - 1), &request))
    goto reject;
  if (!lb_operation_connection_scope(request.op)) {
    uint8_t name[16];
    if (!lb_auth_namespace(request.public_key, name) ||
        !namespace_channel_bind(&c->peer, request.channel, name)) goto reject;
    ch = namespace_channel(&c->peer);
  }
  switch (request.op) {
  case lb_op_begin_owner_sync:
    begun = lb_begin_owner_sync_begin(&c->task, ch, &request); break;
  case lb_op_continue_reconciliation:
    begun = lb_continue_reconciliation_begin(&c->task, ch, &request); break;
  case lb_op_fetch_records:
    begun = lb_fetch_records_begin(&c->task, ch, &request); break;
  case lb_op_apply_records_at_revision:
    begun = lb_apply_records_at_revision_begin(&c->task, ch, &request, LB_WRITE_WINDOW); break;
  case lb_op_stream_record_changes:
    begun = lb_stream_record_changes_begin(&c->task, ch, &request, LB_WRITE_WINDOW); break;
  case lb_op_close_channel:
    begun = lb_close_channel_begin(&c->task, &request); break;
  case lb_op_heartbeat:
    begun = lb_heartbeat_begin(&c->task); break;
  case lb_op_subscribe_to_records:
    begun = lb_subscribe_to_records_begin(&c->task, namespaces, epoch, ch, &request); break;
  default: break;
  }
  if (!begun) goto reject;
  goto done;
reject:
  lb_reply_rejected(&c->task.reply);
  c->task.phase = lb_task_done;
done:
  buffer_return(s->connections.buffers, &c->input);
  c->received = 0;
  c->body = false;
  connection_account(&s->connections, c);
  if (!connection_budget(&s->connections)) c->closing = true;
}
static bool finish(loop *s, client *c) {
  channel *ch = namespace_channel(&c->peer);
  bool finished;
  switch (c->task.operation) {
  case lb_op_begin_owner_sync:
    finished = lb_begin_owner_sync_finish(&c->task, ch, s->connections.subscribers); break;
  case lb_op_continue_reconciliation:
    finished = lb_continue_reconciliation_finish(&c->task, ch, s->connections.subscribers); break;
  case lb_op_fetch_records:
    finished = lb_fetch_records_finish(&c->task, ch, s->connections.subscribers); break;
  case lb_op_apply_records_at_revision:
    finished = lb_apply_records_at_revision_finish(&c->task, ch, s->connections.subscribers); break;
  case lb_op_stream_record_changes:
    finished = lb_stream_record_changes_finish(&c->task, ch, s->connections.subscribers); break;
  case lb_op_close_channel:
    finished = lb_close_channel_finish(&c->task, &c->peer, s->connections.subscribers); break;
  case lb_op_heartbeat:
    finished = lb_heartbeat_finish(&c->task); break;
  case lb_op_subscribe_to_records:
    finished = lb_subscribe_to_records_finish(&c->task, &c->peer, s->connections.subscribers); break;
  default: finished = lb_reply_valid(&c->task.reply); break;
  }
  if (!finished) return false;
  bool ok = c->task.reply_kind == lb_reply_silent ||
      frame(s, c, c->task.reply_kind == lb_reply_event ? 0 : c->request_id,
            c->channel_id, lb_buffer_bytes(&c->task.reply));
  c->peer.active = SIZE_MAX;
  return ok;
}
static bool event(loop *s, client *c, const lb_directory *namespaces, uint64_t epoch) {
  lb_buffer reply = CT_ZERO;
  uint32_t channel_id = 0;
  bool ok = subscriber_next(s->connections.subscribers, &c->peer, namespaces, epoch, &reply, &channel_id);
  if (ok && reply.size) ok = frame(s, c, 0, channel_id, lb_buffer_bytes(&reply));
  lb_buffer_free(&reply);
  return ok;
}
/* Both transports feed exactly the same length-prefixed application stream. */
static bool input_length(loop *s, client *c) {
  if (!c->body && c->received == sizeof(c->header)) {
    uint32_t length = 0;
    for (size_t i = 0; i < sizeof(c->header); ++i) length = (length << 8) | c->header[i];
    if (length < 13 || length > LB_MAX_FRAME || !buffer_borrow(&s->connections, &c->input, length)) return false;
    c->input.size = length;
    c->received = 0;
    c->body = true;
  }
  return true;
}
static bool websocket_output(loop *s, client *c, const void *data, size_t size, int opcode, bool close) {
  uint8_t header[10];
  size_t prefix = opcode ? lb_ws_header(header, (uint8_t)opcode, size) : 0;
  if (!buffer_borrow(&s->connections, &c->output, prefix + size)) return false;
  if (prefix) memcpy(c->output.data, header, prefix);
  memcpy(c->output.data + prefix, data, size);
  c->output.size = prefix + size;
  c->sent = 0;
  c->websocket->raw_output = true;
  c->websocket->close_after_send = close;
  return true;
}
static bool websocket_input(loop *s, client *c) {
  lb_websocket *w = c->websocket;
  if (!w->upgraded) {
    size_t end = 0;
    char response[256];
    int status;
    for (size_t i = 3; i < w->count; ++i)
      if (!memcmp(w->buffer + i - 3, "\r\n\r\n", 4)) { end = i + 1; break; }
    if (!end && w->count < sizeof(w->buffer)) return true;
    status = end ? lb_ws_upgrade(w->buffer, end, response) : 431;
    if (status != 101) {
      snprintf(response, sizeof(response), "HTTP/1.1 %d Upgrade Rejected\r\nConnection: close\r\nSec-WebSocket-Version: 13\r\nContent-Length: 0\r\n\r\n", status);
    } else { w->upgraded = true; w->at = end; }
    return websocket_output(s, c, response, strlen(response), 0, status != 101);
  }
  while (w->at < w->count && !(c->body && c->received == c->input.size)) {
    if (w->remaining && w->opcode < 8) {
      size_t room = (c->body ? c->input.size : sizeof(c->header)) - c->received;
      size_t amount = w->count - w->at;
      uint8_t *destination = c->body ? c->input.data : c->header;
      if (amount > room) amount = room;
      amount = lb_ws_data(w, w->buffer + w->at, destination + c->received, amount);
      w->at += amount;
      c->received += amount;
      if (!input_length(s, c)) return false;
      continue;
    }
    uint8_t data = 0;
    int event = lb_ws_byte(w, w->buffer[w->at++], &data, (uint64_t)LB_MAX_FRAME + 4);
    if (event == 1) {
      if (c->body) c->input.data[c->received++] = data;
      else c->header[c->received++] = data;
      if (!input_length(s, c)) return false;
    } else if (event < 0) {
      uint8_t reason[2] = {(uint8_t)((-event) >> 8), (uint8_t)(-event)};
      return websocket_output(s, c, reason, 2, 8, true);
    } else if (event == 8 || event == 9) {
      return websocket_output(s, c, w->control, w->control_size, event == 9 ? 10 : 8, event == 8);
    }
  }
  if (w->at == w->count) w->at = w->count = 0;
  return true;
}
static void advance(loop *s, client *c, lb_directory *namespaces, uint64_t epoch,
                     lb_store *records, size_t *active_writers) {
  struct io_uring_sqe *sqe;
  size_t length;
  void *buffer;
  if (c->fd < 0)
    return;
  if (stopping || now_ms() > c->deadline)
    c->closing = true;
  if (c->closing) {
    close_client(s, c, namespaces, active_writers);
    return;
  }
  if (c->websocket && !c->receiving && !c->task_active && !client_output_size(c) &&
      !websocket_input(s, c)) {
    close_client(s, c, namespaces, active_writers);
    return;
  }
  if (!c->sending && !client_output_size(c) && !c->task_active && c->body && c->received == c->input.size)
    dispatch(s, c, namespaces, epoch);
  if (c->closing) {
    close_client(s, c, namespaces, active_writers);
    return;
  }
  if (c->task_active) {
    if (c->task.abort_status) {
      storage_wait_remove(&s->connections, c);
      resume_task(&c->task, namespaces, epoch, records, active_writers, 0);
      publish_changes(&c->task, s->connections.subscribers);
    }
    /* Submit already-reserved requests even while parked for more capacity.
     * This also resubmits partial/EAGAIN writes without needing a free slot. */
    if (lb_task_io_ready(&c->task)) {
      if (c->file_capacity < c->task.pending.extra_count) {
        completion *tags;
        if (c->file_pending) abort();
        tags = realloc(c->extra_files, c->task.pending.extra_count * sizeof(*tags));
        if (!tags) { close_client(s, c, namespaces, active_writers); return; }
        c->extra_files = tags;
        c->file_capacity = c->task.pending.extra_count;
        connection_account(&s->connections, c);
        if (!connection_budget(&s->connections)) { close_client(s, c, namespaces, active_writers); return; }
      }
      for (size_t slot = 0; slot <= c->task.pending.extra_count; ++slot) {
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
    if (c->storage_queued || (!c->task.pending.extra_count && c->file_pending)) return;
    if (c->task.phase != lb_task_done)
      resume_task(&c->task, namespaces, epoch, records, active_writers, 64);
    publish_changes(&c->task, s->connections.subscribers);
    connection_account(&s->connections, c);
    if (!connection_budget(&s->connections)) {
      close_client(s, c, namespaces, active_writers);
      return;
    }
    if (c->task.waiting_capacity) {
      storage_wait_push(&s->connections, c);
    }
    if (c->task.phase == lb_task_done) {
      if (c->file_pending) abort();
      storage_wait_remove(&s->connections, c);
      bool ok = finish(s, c);
      dispose_task(&c->task, namespaces, active_writers, s->connections.subscribers);
      c->task_active = false;
      if (!ok) {
        close_client(s, c, namespaces, active_writers);
        return;
      }
    }
  }
  if (c->task_active) {
    if (lb_task_io_ready(&c->task) || (!c->file_pending && !c->storage_queued))
      queue_push(&s->connections.ready, c);
    return;
  }
  if (!c->sending && !client_output_size(c) && (!c->websocket || c->websocket->upgraded) && !event(s, c, namespaces, epoch)) {
    close_client(s, c, namespaces, active_writers);
    return;
  }
  if (client_output_size(c) && !c->sending) {
    sqe = client_sqe(s, c);
    if (sqe) {
      if (!c->websocket && !c->peer.output_update) {
        io_uring_prep_send(sqe, c->fd, c->output.data + c->sent, c->output.size - c->sent,
                           MSG_NOSIGNAL);
      } else {
        /* Prefix and vectors remain pinned until the CQE, including partial
         * sends. Shared subscription bodies are never copied for WebSockets. */
        size_t offset = c->sent, count = 0;
        struct iovec parts[3];
        if (c->websocket && !c->websocket->raw_output && !c->websocket_prefix_size)
          c->websocket_prefix_size = lb_ws_header(c->websocket_prefix, 2, client_output_size(c));
        parts[0] = (struct iovec){c->websocket_prefix, c->websocket_prefix_size};
        parts[1] = c->peer.output_update ? (struct iovec){c->peer.output_header, sizeof(c->peer.output_header)} :
                                     (struct iovec){c->output.data, c->output.size};
        parts[2] = c->peer.output_update ? (struct iovec){c->peer.output_update->data, c->peer.output_update->size} :
                                     (struct iovec){NULL, 0};
        for (size_t i = 0; i < 3; ++i) {
          if (offset >= parts[i].iov_len) { offset -= parts[i].iov_len; continue; }
          c->output_vectors[count++] = (struct iovec){(uint8_t *)parts[i].iov_base + offset,
                                                      parts[i].iov_len - offset};
          offset = 0;
        }
        memset(&c->output_message, 0, sizeof(c->output_message));
        c->output_message.msg_iov = c->output_vectors;
        c->output_message.msg_iovlen = count;
        io_uring_prep_sendmsg(sqe, c->fd, &c->output_message, MSG_NOSIGNAL);
      }
      io_uring_sqe_set_data(sqe, &c->send);
      c->sending = true;
    }
  }
  length = c->body ? c->input.size : sizeof(c->header);
  if (c->websocket && c->websocket->upgraded && c->websocket->at < c->websocket->count &&
      !c->task_active && !client_output_size(c)) {
    queue_push(&s->connections.ready, c);
    return;
  }
  if (!c->task_active && !client_output_size(c) && !c->receiving && c->received < length) {
    sqe = client_sqe(s, c);
    if (sqe) {
      buffer = c->body ? (void *)(c->input.data + c->received) : (void *)(c->header + c->received);
      size_t amount = length - c->received;
      if (c->websocket) {
        lb_websocket *w = c->websocket;
        buffer = w->buffer + w->count;
        amount = sizeof(w->buffer) - w->count;
      }
      io_uring_prep_recv(sqe, c->fd, buffer, amount, 0);
      io_uring_sqe_set_data(sqe, &c->receive);
      c->receiving = true;
    }
  }
  if (!client_output_size(c) && c->peer.dirty_head && !c->queue)
    queue_push(&s->connections.ready, c);
}
static void complete(loop *s, completion *tag, int result, lb_directory *namespaces, size_t *active_writers) {
  client *c;
  if (tag->kind == op_accept) {
    listener *l = tag == &s->listeners[1].accept ? &s->listeners[1] : &s->listeners[0];
    int no_delay = 1;
    l->accepting = false;
    if (result < 0) {
      if (!stopping) l->accept_retry = now_ms() + 100;
      return;
    }
    if (stopping || setsockopt(result, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay))) {
      close(result);
      return;
    }
    c = connection_budget(&s->connections) ? connection_take(&s->connections) : NULL;
    if (!c) {
      close(result);
      l->accept_retry = now_ms() + 100;
      return;
    }
    c->fd = result;
    if (l == &s->listeners[1]) {
      c->websocket = calloc(1, sizeof(*c->websocket));
      c->websocket_bytes = sizeof(*c->websocket);
      if (!c->websocket) { reset(s, c, namespaces, active_writers); return; }
      connection_account(&s->connections, c);
    }
    if (!connection_budget(&s->connections)) {
      reset(s, c, namespaces, active_writers);
      l->accept_retry = now_ms() + 100;
      return;
    }
    timer_touch(&s->connections, c, now_ms() + 30000);
    queue_push(&s->connections.ready, c);
    return;
  }
  if (tag->kind == op_cancel_accept) {
    listener *l = tag == &s->listeners[1].cancel_accept ? &s->listeners[1] : &s->listeners[0];
    l->cancelling_accept = false;
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
      c->task.abort_status = lb_abort_reject;
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
      if (c->peer.output_update) output_update_release(s->connections.subscribers, &c->peer);
      else buffer_return(s->connections.buffers, &c->output);
      c->sent = 0;
      c->websocket_prefix_size = 0;
      if (c->websocket) {
        c->websocket->raw_output = false;
        if (c->websocket->close_after_send) c->closing = true;
      }
    }
    return;
  }
  if (c->websocket) c->websocket->count += (size_t)result;
  else {
    c->received += (size_t)result;
    if (!input_length(s, c)) c->closing = true;
  }
}
static void run(loop *s, lb_directory *namespaces, uint64_t epoch, lb_store *records, size_t *active_writers) {
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
      if (tag) complete(s, tag, result, namespaces, active_writers);
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
    for (size_t i = 0; i < 2; ++i) {
      listener *l = &s->listeners[i];
      if (stopping && l->accepting && !l->accept_cancel_requested) {
        struct io_uring_sqe *sqe = io_uring_get_sqe(&s->ring);
        if (sqe) {
          io_uring_prep_cancel(sqe, &l->accept, 0);
          io_uring_sqe_set_data(sqe, &l->cancel_accept);
          l->cancelling_accept = l->accept_cancel_requested = true;
        }
      }
      submit_accept(s, l);
    }
    for (size_t turns = 0; turns < 256; ++turns) {
      client *c = connection_next(cs, records);
      if (!c) break;
      advance(s, c, namespaces, epoch, records, active_writers);
      if (c->fd >= 0) {
        connection_account(cs, c);
        if (!connection_budget(cs)) {
          c->closing = true;
          queue_push(&cs->ready, c);
        }
      }
    }
    if (stopping && !cs->count && !s->listeners[0].accepting && !s->listeners[0].cancelling_accept &&
        !s->listeners[1].accepting && !s->listeners[1].cancelling_accept) return;
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
        (cs->storage_head && records->pending_count < records->pending_limit)) continue;
    now = now_ms();
    /* A bounded sleep also services POSIX shutdown signals if SA_RESTART is
     * enabled. No live-client scan is performed on this periodic wakeup. */
    delay = 1000;
    if (cs->timer_count) {
      uint64_t due = cs->timers[0]->deadline;
      if (due <= now) delay = 0;
      else if (due - now < delay) delay = due - now;
    }
    for (size_t i = 0; i < 2; ++i) {
      listener *l = &s->listeners[i];
      if (l->fd >= 0 && !l->accepting && !stopping) {
        uint64_t retry = l->accept_retry > now ? l->accept_retry - now : 0;
        if (retry < delay) delay = retry;
      }
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
  out->websocket_port = -1;
  out->memory_budget = 256 * 1024 * 1024;
  for (i = 1; i < argc; i += 2) {
    if (i + 1 == argc)
      return false;
    if (!strcmp(argv[i], "--values"))
      out->values = argv[i + 1];
    else if (!strcmp(argv[i], "--bind"))
      out->bind = argv[i + 1];
    else if (!strcmp(argv[i], "--port") || !strcmp(argv[i], "--ws-port")) {
      char *end = NULL;
      long port;
      errno = 0;
      port = strtol(argv[i + 1], &end, 10);
      if (errno || *end || end == argv[i + 1] || port < 0 || port > 65535)
        return false;
      if (!strcmp(argv[i], "--ws-port")) out->websocket_port = (int)port;
      else out->port = (int)port;
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
  lb_store records = CT_ZERO;
  /* The store owns index memory; operations borrow this directory by argument. */
  lb_directory *namespaces = &records.namespaces;
  size_t active_writers = 0;
  buffer_pool buffers = CT_ZERO;
  lb_subscribers subscribers = CT_ZERO;
  loop s = CT_ZERO;
  struct io_uring_params params = CT_ZERO;
  struct sockaddr_in address = CT_ZERO;
  socklen_t size = sizeof(address);
  uint64_t nonce;
  int reuse = 1, result = 1;
  bool ring_open = false, records_open = false;
  for (size_t i = 0; i < 2; ++i) {
    s.listeners[i].fd = -1;
    s.listeners[i].accept.kind = op_accept;
    s.listeners[i].cancel_accept.kind = op_cancel_accept;
  }
  if (!parse_options(argc, argv, &args)) {
    fprintf(stderr, "expected --values path [--bind address] [--port number] [--ws-port number] [--memory-budget bytes]\n");
    return 1;
  }
  s.connections.memory_budget = args.memory_budget;
  s.connections.buffers = &buffers;
  s.connections.subscribers = &subscribers;
  subscribers.context = &s.connections;
  subscribers.reserve = connection_delivery_reserve;
  subscribers.wake = connection_delivery_wake;
  if (!lb_auth_initialize() || !boot_nonce(&nonce)) {
    fprintf(stderr, "cryptography initialization failed\n");
    goto done;
  }
  if (lb_store_create(&records, args.values, UINT64_C(1) << 30, 64, 0, NULL, ct_system_allocate).code != lb_error_ok) {
    fprintf(stderr, "cache creation failed\n");
    goto done;
  }
  records_open = true;

  params.flags =
      IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_DEFER_TASKRUN;
  if (io_uring_queue_init_params(64, &s.ring, &params) < 0) {
    fprintf(stderr, "io_uring setup failed\n");
    goto done;
  }
  ring_open = true;
  for (size_t i = 0; i < 2; ++i) {
    listener *l = &s.listeners[i];
    int port = i ? args.websocket_port : args.port;
    if (port < 0) continue;
    l->fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (l->fd < 0) { perror("socket"); goto done; }
    setsockopt(l->fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, args.bind, &address.sin_addr) != 1 ||
        bind(l->fd, (struct sockaddr *)&address, sizeof(address)) || listen(l->fd, SOMAXCONN)) {
      perror("listen"); goto done;
    }
    getsockname(l->fd, (struct sockaddr *)&address, &size);
    printf("Littlebear %s listening %s:%u\n", i ? "WebSocket" : "TCP", args.bind, (unsigned)ntohs(address.sin_port));
  }
  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, stop);
  signal(SIGINT, stop);
  fflush(stdout);
  run(&s, namespaces, nonce, &records, &active_writers);
  result = 0;
done:
  for (size_t i = 0; i < 2; ++i)
    if (s.listeners[i].fd >= 0) close(s.listeners[i].fd);
  if (ring_open)
    io_uring_queue_exit(&s.ring);
  assert(!active_writers);
  if (records_open) {
    lb_store_result rc = lb_store_destroy(&records);
    assert(rc.code == lb_error_ok);
    (void)rc;
  }
  connections_destroy(&s.connections);
  buffers_trim(&buffers);
  assert(!buffers.allocated);
  ct_map_free(&subscribers.watches);
  return result;
}
