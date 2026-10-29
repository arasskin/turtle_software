#ifndef LB_MODELS_H
#define LB_MODELS_H
#include "../containers/base.h"
#include "../containers/buffer.h"
#include "../containers/map.h"
/* Shared specification of Littlebear's world: values, shapes and rules.
 * Any component may import this file. Parsing, execution, allocation policy and
 * operation-private progress remain with the components that own them. */
enum {
  LB_MAX_FRAME = 8 * 1024 * 1024,
  LB_MAX_PACKET = LB_MAX_FRAME - 2048,
  LB_MAX_RECORDS = 1000000,
  LB_NAMESPACE_BYTES = 16,
  LB_PUBLIC_KEY_BYTES = 32,
  LB_CHALLENGE_BYTES = 32,
  LB_SIGNATURE_BYTES = 64,
  LB_REPLY_HEADER_BYTES = 17,
  LB_CHALLENGE_FLAG = 0x80
};
static const uint8_t lb_envelope_magic[] = {'L', 'B', 'S', 5};

/* Byte views borrow memory; buffers own their allocation. */
typedef struct lb_bytes {
  const uint8_t *data;
  size_t size;
} lb_bytes;
typedef ct_buffer lb_buffer;
static inline lb_bytes lb_bytes_make(const void *p, size_t n) {
  lb_bytes b = {(const uint8_t *)p, n};
  return b;
}
static inline lb_bytes lb_buffer_bytes(const lb_buffer *b) {
  return lb_bytes_make(b->data, b->size);
}
static inline bool lb_bytes_equal(lb_bytes a, lb_bytes b) {
  return a.size == b.size && (!a.size || memcmp(a.data, b.data, a.size) == 0);
}

/* LBS5 wire IDs. Keep these values stable across transports. */
typedef enum lb_operation_id {
  lb_op_begin_owner_sync = 0,
  lb_op_continue_reconciliation = 1,
  lb_op_fetch_records = 2,
  lb_op_apply_records_at_revision = 3,
  /* Wire ID 4 is retired; subscription modes now use operation 8. */
  lb_op_close_channel = 5,
  lb_op_stream_record_changes = 6,
  lb_op_heartbeat = 7,
  lb_op_subscribe_to_records = 8,
  lb_operation_limit = 9
} lb_operation_id;
static inline bool lb_operation_supported(unsigned op) {
  switch (op) {
  case lb_op_begin_owner_sync:
  case lb_op_continue_reconciliation:
  case lb_op_fetch_records:
  case lb_op_apply_records_at_revision:
  case lb_op_close_channel:
  case lb_op_stream_record_changes:
  case lb_op_heartbeat:
  case lb_op_subscribe_to_records:
    return true;
  default:
    return false;
  }
}

/* These are protocol rules; decoding the bytes is server's responsibility. */
static inline bool lb_operation_signed(unsigned op) {
  return op == lb_op_apply_records_at_revision || op == lb_op_stream_record_changes;
}
static inline bool lb_operation_connection_scope(unsigned op) {
  return op == lb_op_heartbeat;
}
static inline bool lb_operation_empty_payload(unsigned op) {
  return op == lb_op_close_channel || op == lb_op_heartbeat;
}

/* Parsed envelope views expire when the receive buffer is returned. Operations
 * copy any payload they retain; signed_bytes is the exact original signed prefix. */
typedef struct lb_request {
  uint32_t id, channel;
  uint8_t op;
  lb_bytes public_key, payload;
  uint64_t epoch, revision;
  lb_bytes challenge, signature, signed_bytes;
  uint64_t sequence;
} lb_request;

/* Response statuses. A challenge flag may accompany an opening response. */
typedef enum lb_reply_status {
  lb_status_ok = 0, lb_status_retry = 1, lb_status_rejected = 2,
  lb_status_absent = 3, lb_status_namespace_changed = 4,
  lb_status_record_changes = 5, lb_status_reconcile_required = 6
} lb_reply_status;
typedef enum lb_reply_kind { lb_reply_response, lb_reply_event, lb_reply_silent } lb_reply_kind;
static inline bool lb_reply_valid(const lb_buffer *reply) {
  return !reply->failed && reply->size >= LB_REPLY_HEADER_BYTES;
}

/* Records borrow the request payload until the server consumes this effect. */
typedef struct lb_publication {
  uint64_t before, after, epoch, fingerprint;
  lb_bytes records;
} lb_publication;

/* Application peer state is independent of sockets, io_uring and WebSockets. */
typedef struct subscription subscription;
typedef struct shared_update shared_update;
typedef struct lb_auth {
  uint8_t challenge[LB_CHALLENGE_BYTES];
  uint64_t sequence;
  bool issued, pushing;
} lb_auth;
typedef struct channel {
  uint32_t id;
  uint8_t name[LB_NAMESPACE_BYTES];
  lb_auth auth;
  uint64_t announced;
  bool subscribed, known, optimistic, needs_reconcile;
  struct subscription *subscription;
} channel;
typedef struct lb_peer {
  channel *channels;
  size_t channel_count, channel_capacity, active;
  ct_map channel_index;
  bool closing;
  subscription *dirty_head, *dirty_tail;
  shared_update *output_update;
  uint8_t output_header[12];
  size_t delivery_bytes;
  void *context; /* opaque scheduler-owned identity */
} lb_peer;
#endif
