#pragma once
#include <cstdlib>
inline void* fiFontMalloc(size_t n) { return std::malloc(n); }
inline void fiFontFree(void* p) { std::free(p); }
