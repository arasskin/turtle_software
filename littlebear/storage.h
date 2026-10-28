#ifndef LB_STORAGE_H
#define LB_STORAGE_H
#include "namespaces.h"
#include "heap.h"
#include <errno.h>
#include <sys/file.h>
typedef enum {
  lb_error_ok,
  lb_error_invalid_argument,
  lb_error_not_open,
  lb_error_namespace_missing,
  lb_error_record_missing,
  lb_error_busy,
  lb_error_no_memory,
  lb_error_capacity,
  lb_error_buffer_too_small,
  lb_error_io_failure,
  lb_error_more_io,
  lb_error_more_work,
  lb_error_cancelled,
  lb_error_invalid_state,
  /* Capacity pressure is distinct from a record conflict. Retry after another
   * request finishes; no storage mutation or request ownership has occurred. */
  lb_error_pending_full
} lb_error;
typedef enum { lb_presence_absent, lb_presence_empty, lb_presence_populated } lb_store_presence;
typedef enum { lb_phase_idle, lb_phase_ready, lb_phase_submitted, lb_phase_finished } lb_phase;
typedef struct lb_store_result {
  lb_error code;
  int system_error;
} lb_store_result;
typedef lb_namespace lb_store_namespace_id;
typedef uint64_t lb_store_record_key;
/* Zero-initialize stores and requests before first use. The store and active
 * requests must remain at stable addresses until their I/O completes. */
typedef struct lb_store_request lb_store_request;
typedef struct lb_store {
  lb_file values;
  ct_pool nodes;
  lb_directory namespaces;
  lb_store_request *pending;
  size_t pending_count, pending_limit;
  bool opened;
} lb_store;
struct lb_store_request {
  lb_store *owner;
  lb_store_request *next;
  lb_store_request **pending_link;
  lb_namespace ns;
  uint64_t key, value, previous, fingerprint;
  void *buffer;
  size_t transferred;
  int release_ticket;
  bool writing;
  lb_phase state;
  lb_store_result outcome;
};
typedef struct lb_store_io_operation {
  int fd;
  uint64_t offset;
  void *buffer;
  uint32_t length;
  bool writing;
} lb_store_io_operation;
static inline bool lb_directory_bad(const lb_directory *t) {
  return !t->initialized;
}
static inline bool lb_directory_contains(lb_directory *t, lb_namespace ns) {
  return lb_directory_get(t, ns) != NULL;
}
static inline bool lb_index_empty(const lb_index *h) {
  return !h->root;
}
static inline void lb_store_release_resources(lb_store *s) {
  size_t i;
  for (i = 0; i < s->namespaces.count; ++i)
    lb_index_destroy(&s->namespaces.entries[i].value);
  lb_directory_destroy(&s->namespaces);
  if (s->values.file_allocator.slab_allocator.start != NULL)
    lb_file_destroy(&s->values);
  ct_pool_destroy(&s->nodes);
  memset(s, 0, sizeof(*s));
  s->values.file_descriptor = -1;
  s->values.has_error = true;
}

// Dedicated disposable cache file, locked before truncation. Call on a fresh or
// destroyed lb_store; it must not subsequently move, because tree headers borrow it.
static inline lb_store_result lb_store_create(lb_store *s, const char *path, uint64_t file_limit,
                                              size_t pending_limit, size_t initial_namespaces,
                                              ct_allocate_fn index_allocate,
                                              ct_allocate_fn namespace_allocate) {
  if (s->opened)
    return (lb_store_result){lb_error_busy, 0};
  if (path == NULL || file_limit == 0 || file_limit > LB_OFFSET_MASK + 1 || pending_limit == 0 ||
      !lb_directory_valid(initial_namespaces) || !namespace_allocate)
    return (lb_store_result){lb_error_invalid_argument, 0};
  s->values = lb_file_create(path, file_limit);
  if (s->values.has_error)
    return (lb_store_result){lb_error_io_failure, errno};
  if (flock(s->values.file_descriptor, LOCK_EX | LOCK_NB) != 0) {
    const int saved = errno;
    lb_store_release_resources(s);
    return (lb_store_result){lb_error_io_failure, saved};
  }
  ct_pool_init(&s->nodes, index_allocate);
  s->namespaces = lb_directory_create(initial_namespaces, namespace_allocate);
  if (lb_directory_bad(&s->namespaces)) {
    lb_store_release_resources(s);
    return (lb_store_result){lb_error_no_memory, 0};
  }
  if (ftruncate(s->values.file_descriptor, 0) != 0) {
    const int saved = errno;
    lb_store_release_resources(s);
    return (lb_store_result){lb_error_io_failure, saved};
  }
  s->pending_limit = pending_limit;
  s->opened = true;
  return (lb_store_result)CT_ZERO;
}

static inline lb_store_result lb_store_destroy(lb_store *s) {
  if (s->pending_count != 0)
    return (lb_store_result){lb_error_busy, 0};
  lb_store_release_resources(s);
  return (lb_store_result)CT_ZERO;
}

static inline bool lb_store_namespace_busy(lb_store *s, lb_store_namespace_id ns) {
  for (lb_store_request *r = s->pending; r != NULL; r = r->next)
    if (lb_namespace_equal(r->ns, ns))
      return true;
  return false;
}

// Different records can issue I/O concurrently, including within one namespace.
// Multiple reads can share a record; mutation waits for all of its reads.
static inline bool lb_store_record_busy(lb_store *s, lb_store_namespace_id ns,
                                        lb_store_record_key key, bool writing) {
  for (lb_store_request *r = s->pending; r != NULL; r = r->next)
    if (lb_namespace_equal(r->ns, ns) && r->key == key && (writing || r->writing))
      return true;
  return false;
}

static inline lb_store_presence lb_store_lookup_namespace(lb_store *s, lb_store_namespace_id ns) {
  lb_index *index = lb_directory_get(&s->namespaces, ns);
  return index == NULL           ? lb_presence_absent
         : lb_index_empty(index) ? lb_presence_empty
                                 : lb_presence_populated;
}

static inline lb_store_result lb_store_create_namespace(lb_store *s, lb_store_namespace_id ns) {
  if (!s->opened)
    return (lb_store_result){lb_error_not_open, 0};
  if (lb_directory_contains(&s->namespaces, ns))
    return (lb_store_result)CT_ZERO;
  return lb_directory_insert(&s->namespaces, ns, lb_index_create(&s->nodes))
             ? (lb_store_result)CT_ZERO
             : (lb_store_result){lb_error_no_memory, 0};
}

static inline lb_store_result lb_store_erase_record(lb_store *s, lb_store_namespace_id ns,
                                                    lb_store_record_key key) {
  if (!s->opened)
    return (lb_store_result){lb_error_not_open, 0};
  lb_index *index = lb_directory_get(&s->namespaces, ns);
  if (index == NULL)
    return (lb_store_result){lb_error_namespace_missing, 0};
  if (lb_store_record_busy(s, ns, key, true))
    return (lb_store_result){lb_error_busy, 0};
  ct_record entry;
  if (!lb_index_find(index, key, &entry))
    return (lb_store_result){lb_error_record_missing, 0};
  const uint64_t value = entry.value;
  const int ticket = lb_heap_prepare(&s->values.file_allocator);
  if (ticket == -1)
    return (lb_store_result){lb_error_no_memory, 0};
  if (lb_index_erase(index, key) != 1) {
    lb_heap_cancel(&s->values.file_allocator, ticket);
    return (lb_store_result){lb_error_no_memory, 0};
  }
  lb_heap_release_prepared(&s->values.file_allocator, lb_voffset(value), lb_vsize(value), ticket);
  return (lb_store_result)CT_ZERO;
}

// Bounded work per call. more_work means some records remain; an allocation
// failure can also leave a partially cleared namespace. Both are safe to retry.
static inline lb_store_result lb_store_clear_namespace(lb_store *s, lb_store_namespace_id ns,
                                                       size_t record_budget) {
  if (!s->opened)
    return (lb_store_result){lb_error_not_open, 0};
  if (record_budget == 0)
    return (lb_store_result){lb_error_invalid_argument, 0};
  lb_index *index = lb_directory_get(&s->namespaces, ns);
  if (index == NULL)
    return (lb_store_result){lb_error_namespace_missing, 0};
  if (lb_store_namespace_busy(s, ns))
    return (lb_store_result){lb_error_busy, 0};
  while (!lb_index_empty(index) && record_budget-- != 0) {
    ct_record first;
    lb_index_select(index, 0, &first);
    const uint64_t key = first.key;
    const lb_store_result erased = lb_store_erase_record(s, ns, key);
    if (erased.code != lb_error_ok)
      return erased;
  }
  return lb_index_empty(index) ? (lb_store_result)CT_ZERO
                               : (lb_store_result){lb_error_more_work, 0};
}

static inline lb_store_result lb_store_remove_namespace(lb_store *s, lb_store_namespace_id ns,
                                                        size_t record_budget) {
  lb_store_result cleared = lb_store_clear_namespace(s, ns, record_budget);
  if (cleared.code == lb_error_ok)
    lb_directory_erase(&s->namespaces, ns);
  return cleared;
}

static inline void lb_store_attach(lb_store *s, lb_store_request *r) {
  r->owner = s;
  r->next = s->pending;
  r->pending_link = &s->pending;
  if (r->next) r->next->pending_link = &r->next;
  s->pending = r;
  ++s->pending_count;
  r->state = lb_phase_ready;
}

static inline lb_store_result lb_store_prepare_read(lb_store *s, lb_store_request *r,
                                                    lb_store_namespace_id ns,
                                                    lb_store_record_key key, void *buffer,
                                                    size_t capacity) {
  if (!s->opened)
    return (lb_store_result){lb_error_not_open, 0};
  if (r->owner != NULL)
    return (lb_store_result){lb_error_busy, 0};
  lb_index *index = lb_directory_get(&s->namespaces, ns);
  if (index == NULL)
    return (lb_store_result){lb_error_namespace_missing, 0};
  if (lb_store_record_busy(s, ns, key, false))
    return (lb_store_result){lb_error_busy, 0};
  if (s->pending_count >= s->pending_limit)
    return (lb_store_result){lb_error_pending_full, 0};
  const uint64_t value = lb_index_get(index, key);
  if (value == 0)
    return (lb_store_result){lb_error_record_missing, 0};
  if (capacity < lb_vsize(value))
    return (lb_store_result){lb_error_buffer_too_small, 0};
  if (buffer == NULL)
    return (lb_store_result){lb_error_invalid_argument, 0};
  memset(r, 0, sizeof(*r));
  r->release_ticket = -1;
  r->ns = ns;
  r->key = key;
  r->value = value;
  r->buffer = buffer;
  lb_store_attach(s, r);
  return (lb_store_result)CT_ZERO;
}

// The fingerprint is the current internal tree summary, supplied by the caller;
// it is not a negentropy record ID. Buffer contents must stay unchanged until done.
static inline lb_store_result lb_store_prepare_write(lb_store *s, lb_store_request *r,
                                                     lb_store_namespace_id ns,
                                                     lb_store_record_key key, const void *buffer,
                                                     size_t length, uint64_t fingerprint) {
  if (!s->opened)
    return (lb_store_result){lb_error_not_open, 0};
  if (r->owner != NULL)
    return (lb_store_result){lb_error_busy, 0};
  if (buffer == NULL || length == 0 || length > LB_MAX_VALUE)
    return (lb_store_result){lb_error_invalid_argument, 0};
  lb_index *index = lb_directory_get(&s->namespaces, ns);
  if (index == NULL)
    return (lb_store_result){lb_error_namespace_missing, 0};
  if (lb_store_record_busy(s, ns, key, true))
    return (lb_store_result){lb_error_busy, 0};
  if (s->pending_count >= s->pending_limit)
    return (lb_store_result){lb_error_pending_full, 0};
  const uint64_t previous = lb_index_get(index, key);
  lb_heap *allocator = &s->values.file_allocator;
  /* Exactly one extent can need freeing: the new one on failure, or the old
   * one on successful replacement. Reserve cleanup before starting I/O. */
  const int release = lb_heap_prepare(allocator);
  if (release == -1)
    return (lb_store_result){lb_error_no_memory, 0};
  lb_offset allocation = lb_heap_reserve(allocator, length);
  if (allocation.has_error) {
    lb_heap_cancel(allocator, release);
    return (lb_store_result){lb_error_capacity, 0};
  }
  memset(r, 0, sizeof(*r));
  r->ns = ns;
  r->key = key;
  r->previous = previous;
  r->value = lb_vpointer(allocation.offset, (uint16_t)(length));
  r->fingerprint = fingerprint;
  r->buffer = (void *)(buffer);
  r->release_ticket = release;
  r->writing = true;
  lb_store_attach(s, r);
  return (lb_store_result)CT_ZERO;
}

// Obtain one operation and mark it submitted. The driver must eventually report
// its completion exactly once, even after disconnect or kernel cancellation.
static inline lb_store_result lb_store_start_io(lb_store_request *r,
                                                lb_store_io_operation *operation) {
  if (r->owner == NULL || r->state != lb_phase_ready)
    return (lb_store_result){lb_error_invalid_state, 0};
  *operation = (lb_store_io_operation){.fd = r->owner->values.file_descriptor,
                                       .offset = lb_voffset(r->value) + r->transferred,
                                       .buffer = (uint8_t *)(r->buffer) + r->transferred,
                                       .length = (uint32_t)(lb_vsize(r->value) - r->transferred),
                                       .writing = r->writing};
  r->state = lb_phase_submitted;
  return (lb_store_result)CT_ZERO;
}

// Internal cleanup: only after the submitted operation has completed, or while
// cancelling a ready lb_store_request that the kernel cannot access.
static inline lb_store_result lb_store_finish(lb_store_request *r, lb_store_result outcome) {
  lb_store *s = r->owner;
  lb_heap *allocator = &s->values.file_allocator;
  if (r->writing) {
    uint64_t retired = outcome.code == lb_error_ok ? r->previous : r->value;
    if (retired)
      lb_heap_release_prepared(allocator, lb_voffset(retired), lb_vsize(retired), r->release_ticket);
    else
      lb_heap_cancel(allocator, r->release_ticket);
  }
  *r->pending_link = r->next;
  if (r->next) r->next->pending_link = r->pending_link;
  --s->pending_count;
  r->owner = NULL;
  r->next = NULL;
  r->pending_link = NULL;
  r->release_ticket = -1;
  r->state = lb_phase_finished;
  r->outcome = outcome;
  return outcome;
}

static inline lb_store_result lb_store_complete_io(lb_store_request *r, int completed_bytes) {
  if (r->owner == NULL || r->state != lb_phase_submitted)
    return (lb_store_result){lb_error_invalid_state, 0};
  if (completed_bytes == -EINTR || completed_bytes == -EAGAIN) {
    r->state = lb_phase_ready;
    return (lb_store_result){lb_error_more_io, 0};
  }
  if (completed_bytes < 0)
    return lb_store_finish(
        r,
        (lb_store_result){completed_bytes == -ECANCELED ? lb_error_cancelled : lb_error_io_failure,
                          completed_bytes == INT_MIN ? EIO : -completed_bytes});
  const size_t remaining = lb_vsize(r->value) - r->transferred;
  if (completed_bytes == 0 || (size_t)(completed_bytes) > remaining)
    return lb_store_finish(r, (lb_store_result){lb_error_io_failure, EIO});
  r->transferred += completed_bytes;
  if ((size_t)(completed_bytes) < remaining) {
    r->state = lb_phase_ready;
    return (lb_store_result){lb_error_more_io, 0};
  }
  if (r->writing) {
    // Re-look up: unrelated namespaces may have grown or repacked the table.
    lb_index *index = lb_directory_get(&r->owner->namespaces, r->ns);
    if (index == NULL)
      return lb_store_finish(r, (lb_store_result){lb_error_invalid_state, 0});
    if (r->previous != 0) {
      if (!lb_index_replace(index, r->key, r->value, r->fingerprint))
        return lb_store_finish(r, (lb_store_result){lb_error_invalid_state, 0});
    } else if (lb_index_set(index, r->key, r->value, r->fingerprint) < 0)
      return lb_store_finish(r, (lb_store_result){lb_error_no_memory, 0});
  }
  return lb_store_finish(r, (lb_store_result)CT_ZERO);
}

// A submitted buffer cannot be released yet. Cancel it through the I/O driver
// and deliver its original completion before destroying the lb_store_request or buffer.
static inline lb_store_result lb_store_cancel(lb_store_request *r) {
  if (r->owner == NULL)
    return (lb_store_result){lb_error_invalid_state, 0};
  if (r->state == lb_phase_submitted)
    return (lb_store_result){lb_error_busy, 0};
  return lb_store_finish(r, (lb_store_result){lb_error_cancelled, 0});
}

#endif
