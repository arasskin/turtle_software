#ifndef DATAFRIEND_NETWORK_H
#define DATAFRIEND_NETWORK_H
#include "db.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct df_network df_network;
typedef struct df_request_timing {
  uint32_t request, channel;
  uint8_t operation;
  uint64_t elapsed_us, queue_us, snapshot_revision, enqueued_us, sent_us;
} df_request_timing;
typedef struct df_network_stats {
  uint64_t connections, requests, exchanges, restarts, signed_batches;
  uint64_t sent, received, reconcile_us, sign_us, apply_us, request_us, request_count;
  uint64_t optimistic_uploads, optimistic_publications, optimistic_fallbacks;
} df_network_stats;
/* Calls and callbacks run on the caller. Stores/identities outlive the network.
 * Callbacks may read views/edit other stores, but must not pump or destroy it.
 * One network owns each attached store's sole reconciliation exchange. */
/* Push/subscribe may append channels at runtime, including in a publication
 * callback. Channel IDs remain stable across growth and reconnect. There is no
 * fixed channel count; allocation and the nonzero uint32 ID space bound it.
 * Detach outside pump/callbacks before closing an attached store. */
typedef void (*df_publication)(void *context, const uint64_t *views, size_t count);
DF_API df_network *df_network_open(const char *numeric_address, uint16_t port, df_buffer *error);
DF_API void df_network_close(void *);
DF_API uint32_t df_network_push(void *, void *store, void *identity, df_buffer *error);
DF_API uint32_t df_network_subscribe(void *, void *store, const char *public_key_hex,
                                     df_publication, void *context, df_buffer *error);
/* Native subscription: reads the public key from the store namespace. Query
 * watches publish automatically; no forwarding callback is needed. */
DF_API uint32_t df_network_follow(df_network *, df_store *, df_buffer *error);
/* Preserves other channels and their IDs. May queue unsubscribe behind in-flight
 * traffic. On success the detached store/identity may be closed immediately.
 * Fails without detaching if queue allocation fails. Not callable inside pump. */
DF_API df_status df_network_detach(df_network *, uint32_t channel, df_buffer *error);
DF_API void df_network_pump(void *, uint64_t budget_us);
enum { DF_NETWORK_READ = 1, DF_NETWORK_WRITE = 2 };
typedef struct df_network_watch {
  int fd;
  unsigned events;
  uint64_t timeout_us; /* Relative; 0 means pending work, UINT64_MAX means no timer. */
} df_network_watch;
/* Query on the network's owning thread after pump or state changes. The fd is
 * borrowed and may change at the next pump. Wake on readiness OR the deadline,
 * then pump and refresh this query. Store writes/attach/online changes require
 * an explicit owner-thread wake; this function does not install callbacks. */
DF_API df_network_watch df_network_prepare(df_network *);
DF_API int df_network_measure(void *, uint64_t heartbeat_ms, df_buffer *error);
DF_API void df_network_online(void *, int enabled);
DF_API int df_network_connected(void *);
DF_API int df_network_settled(void *);
DF_API const char *df_network_error(void *);
DF_API const df_network_stats *df_network_statistics(void *);
DF_API const df_request_timing *df_network_samples(void *, size_t *count);
DF_API const uint8_t *df_identity_public(void *);
#ifdef __cplusplus
}
#include "cpp/network.h"
#endif
#endif
