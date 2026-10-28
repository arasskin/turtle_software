#pragma once
#include <stddef.h>
#include <stdint.h>
#if defined(_WIN32)
#define DF_API __declspec(dllexport)
#else
#define DF_API __attribute__((visibility("default"),used))
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef struct df_buffer { uint8_t* data; size_t size; } df_buffer;
DF_API void* df_alloc(size_t size);
DF_API void df_free(void* memory);
// Named identity: load/create in the platform secure store. Returns opaque
// handle + owned 32-byte public key; null + error on failure. No seed export.
// ephemeral=1 explicitly creates an unpersisted test identity; 0 is production.
DF_API void* df_identity_open(const uint8_t* alias, size_t size, int ephemeral, df_buffer* reply);
// Only signs an LBS5 write envelope matching this key and the attribute schema.
// Returns owned 64-byte Ed25519 signature, or error. Calls must be serialized.
DF_API int df_identity_sign(void* identity, const uint8_t* message, size_t size, df_buffer* reply);
// Wipes the in-memory key. Does not delete the stored identity.
DF_API void df_identity_close(void* identity);
// Owned UTF-8 fingerprint of the generated attribute schema. 0 success.
DF_API int df_attribute_schema(df_buffer* reply);
// Return null and fill error on failure. Every successful open must be closed.
// Durability: 0 = buffered, 1 = sync, 2 = async flush.
DF_API void* df_open(const uint8_t* directory, size_t size, int durable, df_buffer* error);
DF_API void* df_open_namespace(const uint8_t* directory, size_t size,
                               const uint8_t* name, size_t name_size, int durable, df_buffer* error);
DF_API void df_close(void* engine);
// Flush and release even on error. Use df_close only for no-throw finalization.
DF_API int df_close_checked(void* engine, df_buffer* error);
// Protocol version 1 (see README). 0 success, 1 error. Caller frees reply.data.
// Calls execute directly on the caller; serialize all engine/result access.
// Blocking, non-leaf FFI. No callbacks into Dart are made.
DF_API int df_call(void* engine, const uint8_t* request, size_t size, df_buffer* reply);
// Shared live query map, updated in place. Callers MUST serialize result reads,
// edits, imports, unsubscribe and close on one application thread. No read locks.
// After unsubscribe/engine close a retained handle keeps its final result.
DF_API void* df_result(void* engine, uint64_t view, df_buffer* error);
// Encoded u64 count. All buffers are caller-freed.
DF_API int df_result_info(void* result, df_buffer* reply);
DF_API int df_result_same(void* left, void* right);
DF_API int df_result_list(void* result, df_buffer* reply);
// Latest evaluated change summary: u8 reset, u8 membership, u32 entity count +
// u32 IDs, u32 attribute count + length-prefixed UTF-8 names. Owned reply.
// Read during the corresponding notification; this is not an event history.
DF_API int df_result_changes(void* result, df_buffer* reply);
// Direct reads: attribute IDs are dictionary metadata, zero means absent.
DF_API uint32_t df_result_attribute(void* result,const uint8_t* name,size_t size);
// 0 present (borrowed buffer), 1 absent, -1 error. Never free this buffer.
// Consume before any edit/close; Dart converts/copies values before returning.
DF_API int df_result_value(void* result,uint32_t entity,uint32_t attribute,df_buffer* borrowed);
// Owned host-endian uint32 entity IDs, sorted by optional int64 attributes then ID.
DF_API int df_result_entities(void* result,uint32_t order,uint32_t secondary,df_buffer* reply);
// One projected entity's names: u32 count + length-prefixed UTF-8 strings.
// Owned reply; missing entity returns an empty list. No values are copied.
DF_API int df_result_attributes(void* result,uint32_t entity,df_buffer* reply);
// find: entity ID, -1 absent, -2 error. count: nonnegative, -1 error.
DF_API int64_t df_result_find(void* result,uint32_t attribute,const uint8_t* value,size_t size);
DF_API int64_t df_result_count_value(void* result,uint32_t attribute,const uint8_t* value,size_t size);
DF_API void df_result_release(void* result);
#ifdef __cplusplus
}
#endif
