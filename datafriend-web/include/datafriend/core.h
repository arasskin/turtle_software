#ifndef DATAFRIEND_CORE_H
#define DATAFRIEND_CORE_H
#include "c_api.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Direct metadata/ownership hooks used by native adapters. All access remains
 * serialized with the store. Views and retained boxes may outlive the store. */
DF_API void *df_open_sized(const uint8_t *, size_t, const uint8_t *, size_t, int, size_t,
                           df_buffer *);
DF_API uint64_t df_hash_bytes(const void *, size_t);
DF_API uint64_t df_hash_record(uint64_t, const void *, size_t);
DF_API uint64_t df_revision(void *);
DF_API uint64_t df_recomputed(void *);
DF_API void df_namespace(void *, const uint8_t **, size_t *);
DF_API void df_track(void *);
DF_API int df_delta(void *, uint64_t, df_buffer *); /* 0 packet, 2 unavailable, 1 error */
DF_API int df_apply_delta(void *, const uint8_t *, size_t, df_buffer *); /* 2 mismatch */
DF_API void df_view_keep(void *);
DF_API void *df_view_empty(const uint8_t *, size_t);
DF_API int df_view_valid(void *);
DF_API size_t df_view_size(void *);
DF_API void df_view_namespace(void *, const uint8_t **, size_t *);
DF_API void *df_view_box(void *, uint64_t);
DF_API void *df_view_at(void *, size_t, uint64_t *);
DF_API void df_box_keep(void *);
DF_API void df_box_drop(void *);
DF_API void df_box_value(void *, const uint8_t **, size_t *, uint64_t *);
#ifdef DATAFRIEND_TESTING
DF_API void
df_test_allocation_failure(int64_t after); /* Fail once after N allocations; -1 disables. */
DF_API void df_test_commit_failure(void *);
DF_API void df_test_flush_failure(void *, int);
DF_API uint64_t df_test_flush_attempts(void *);
DF_API void df_test_poison_view(void *);
#endif
#ifdef __cplusplus
}
#endif
#endif
