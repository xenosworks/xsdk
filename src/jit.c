#include "xsdk/jit.h"

#include <assert.h>
#include <stdint.h>

// Defined even for an empty pool by src/xsdk.ld.
extern unsigned char xsdk_jit_start[];
extern unsigned char xsdk_jit_end[];
void xsdk_jit_flush_lines(uintptr_t first, uintptr_t last);

void xsdk_jit_pool(void **base, size_t *bytes)
{
	*base = xsdk_jit_start;
	*bytes = (uintptr_t)xsdk_jit_end - (uintptr_t)xsdk_jit_start;
}

void xsdk_code_flush(const void *start, size_t bytes)
{
	if (!bytes)
		return;
	assert(bytes - 1 <= UINTPTR_MAX - (uintptr_t)start);
	// Xenon's 128-byte lines and the self-modifying-code sequence (libxenon:drivers/ppc/cache.S:1,55-70).
	const uintptr_t first = (uintptr_t)start & ~(uintptr_t)127;
	const uintptr_t last = ((uintptr_t)start + bytes - 1) & ~(uintptr_t)127;
	xsdk_jit_flush_lines(first, last);
}
