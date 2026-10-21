#include "xsdk/jit.h"

#include <assert.h>
#include <stdint.h>

// Xenon's cache line (libxenon:drivers/ppc/cache.S:1).
enum { CACHE_LINE_BYTES = 128 };

// Defined even for an empty pool by src/xsdk.ld.
extern unsigned char xsdk_jit_start[];
extern unsigned char xsdk_jit_end[];

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
	const uintptr_t first = (uintptr_t)start & ~(uintptr_t)(CACHE_LINE_BYTES - 1);
	const uintptr_t last = ((uintptr_t)start + bytes - 1) & ~(uintptr_t)(CACHE_LINE_BYTES - 1);
	// PowerPC Book II v2.01, section 1.8, p. 11; tests/probe_exec.c passed this sequence on the console.
	for (uintptr_t line = first;; line += CACHE_LINE_BYTES) {
		__asm__ volatile("dcbst 0, %0" : : "r"(line) : "memory");
		if (line == last)
			break;
	}
	__asm__ volatile("sync" : : : "memory");
	for (uintptr_t line = first;; line += CACHE_LINE_BYTES) {
		__asm__ volatile("icbi 0, %0" : : "r"(line) : "memory");
		if (line == last)
			break;
	}
	__asm__ volatile("sync" : : : "memory");
	__asm__ volatile("isync" : : : "memory");
}
