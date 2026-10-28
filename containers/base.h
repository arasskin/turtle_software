#ifndef CT_BASE_H
#define CT_BASE_H
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define CT_MIN(a, b) ((a) < (b) ? (a) : (b))
#define CT_MAX(a, b) ((a) > (b) ? (a) : (b))
/* Shared C99 primitives; also usable by the legacy C++ adapters. */
#ifdef __cplusplus
#define CT_ZERO                                                                                    \
  {                                                                                                \
  }
#else
#define CT_ZERO {0}
#endif
static inline unsigned ct_ctz32(uint32_t n) { return n ? (unsigned)__builtin_ctz(n) : 32; }
static inline unsigned ct_clz32(uint32_t n) { return n ? (unsigned)__builtin_clz(n) : 32; }
typedef void *(*ct_allocate_fn)(size_t alignment, size_t bytes);
#endif
