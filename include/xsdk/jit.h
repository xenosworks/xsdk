#ifndef XSDK_JIT_H
#define XSDK_JIT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// The title's .jit input sections, or zero bytes when none; the title manages allocation within the pool.
void xsdk_jit_pool(void **base, size_t *bytes);
// Cached memory, no address wrap; flush intersecting lines, or do nothing for zero bytes.
// After publishing to another thread, that consumer must run isync before execution (docs/GUIDE.md).
void xsdk_code_flush(const void *start, size_t bytes);

#ifdef __cplusplus
}
#endif
#endif
