#pragma once
#include <cstdio>
#include <cstdlib>

namespace testing { inline char context[160] = {}; }

#define CHECK(condition) do { \
  if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
    std::fprintf(stderr, "%s\n", testing::context); \
    std::abort(); \
  } \
} while (false)
