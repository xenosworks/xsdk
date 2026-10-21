#include "tests.h"

#include <errno.h>
#include <malloc.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

#include "xsdk/gpu.h"
#include "xsdk/jit.h"
#include "xsdk/log.h"

enum { PAGE_BYTES = 4096, MAP_BYTES = 256 * 1024, HEAP_BYTES = 32 * 1024 * 1024, CACHE_LINE_BYTES = 128 };

static uint32_t available(void)
{
	MM_QUERY_STATISTICS_RESULT stats = { .size = sizeof(stats) };
	if (!NT_SUCCESS(MmQueryStatistics(&stats))) {
		check("pages: query free pages", false);
		exit(1);
	}
	return stats.title.available_pages;
}

static void mappings(void)
{
	volatile uint32_t *words = mmap(NULL, MAP_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (words == MAP_FAILED) {
		check("pages: anonymous mapping", false);
		return;
	}
	bool zero = true;
	for (uint32_t i = 0; i < MAP_BYTES / sizeof(*words); i++) {
		zero &= words[i] == 0;
		words[i] = i + 1;
	}
	check("pages: anonymous pages are zeroed and accept writes",
		zero && words[0] == 1 && words[MAP_BYTES / 4 - 1] == MAP_BYTES / 4);
	const int ro = mprotect((void *)words, PAGE_BYTES, PROT_READ);
	const uint32_t read_only = MmQueryAddressProtect((uint32_t)(uintptr_t)words);
	const uint32_t first = words[0];
	const int rw = mprotect((void *)words, PAGE_BYTES, PROT_READ | PROT_WRITE);
	if (!rw)
		words[0] = 42;
	check("pages: read-only protection reads back and restores writable",
		ro == 0 && (read_only & 0xFF) == PAGE_READONLY && first == 1 && rw == 0 && words[0] == 42);
	const int none = mprotect((void *)words, PAGE_BYTES, PROT_NONE);
	const uint32_t inaccessible = MmQueryAddressProtect((uint32_t)(uintptr_t)words);
	const int restore = mprotect((void *)words, PAGE_BYTES, PROT_READ | PROT_WRITE);
	check("pages: no-access protection restores to writable",
		none == 0 && (inaccessible & 0xFF) == PAGE_NOACCESS && restore == 0);
	check("pages: executable protection is refused",
		mprotect((void *)words, PAGE_BYTES, PROT_READ | PROT_EXEC) == -1 && errno == ENOTSUP);
	if (restore != 0)
		exit(1);
	const uint32_t before = available();
	unsigned char *base = (unsigned char *)(uintptr_t)words;
	const int middle = munmap(base + PAGE_BYTES, PAGE_BYTES);
	const bool hole = mprotect(base + PAGE_BYTES, PAGE_BYTES, PROT_READ) == -1 && errno == ENOMEM;
	const bool edges = words[0] == 42 && words[2 * PAGE_BYTES / 4] == 2 * PAGE_BYTES / 4 + 1;
	const int again = munmap(base + PAGE_BYTES, PAGE_BYTES);
	check("pages: partial unmap preserves neighbours and leaves a hole", middle == 0 && hole && edges && again == 0);
	const int last = munmap(base, MAP_BYTES);
	const uint32_t after = available();
	xsdk_log_write("pages: unmap free pages %u -> %u", (unsigned)before, (unsigned)after);
	check("pages: unmap returns committed pages to the kernel", last == 0 && after >= before + MAP_BYTES / PAGE_BYTES);
	check("pages: a repeated unmap of released memory succeeds", munmap(base, MAP_BYTES) == 0);
}

static void refused(void)
{
	check("pages: files, fixed and shared mappings are refused",
		mmap(NULL, PAGE_BYTES, PROT_READ, MAP_PRIVATE, 0, 0) == MAP_FAILED && errno == ENOTSUP &&
			mmap(NULL, PAGE_BYTES, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED &&
			errno == ENOTSUP && mmap(NULL, PAGE_BYTES, PROT_READ, MAP_SHARED | MAP_ANONYMOUS, -1, 0) == MAP_FAILED &&
			errno == ENOTSUP);
	check("pages: zero length, overflow and unaligned ranges are refused",
		mmap(NULL, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == EINVAL &&
			mmap(NULL, SIZE_MAX, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == EINVAL &&
			munmap((void *)1, PAGE_BYTES) == -1 && errno == EINVAL);
	check("pages: executable mappings are refused",
		mmap(NULL, PAGE_BYTES, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED &&
			errno == ENOTSUP);
}

static void physical_protection(void)
{
	volatile void *physical = xsdk_gpu_alloc(65536);
	if (!physical) {
		check("pages: allocate physical memory", false);
		return;
	}
	const uint32_t address = (uint32_t)(uintptr_t)physical;
	const uint32_t original = MmQueryAddressProtect(address);
	MmSetAddressProtect((void *)physical, 65536, PAGE_READONLY);
	const uint32_t ro = MmQueryAddressProtect(address);
	MmSetAddressProtect((void *)physical, 65536, original);
	const uint32_t restored = MmQueryAddressProtect(address);
	check("pages: physical protection changes and restores", (ro & 0xFF) == PAGE_READONLY && restored == original);
	xsdk_gpu_free(physical);
}

static void heap_return(void)
{
	// newlib's default top padding is zero; keep it explicit for a full 32 MB return.
	if (!mallopt(M_TOP_PAD, 0)) {
		check("pages: set malloc trim padding", false);
		return;
	}
	const uintptr_t original_break = (uintptr_t)sbrk(0);
	const uint32_t original_pages = available();
	volatile uint32_t *block = malloc(HEAP_BYTES);
	if (!block) {
		check("pages: allocate heap buffer", false);
		return;
	}
	// Volatile page writes keep GCC from deleting the unused malloc/free pair.
	for (uint32_t offset = 0; offset < HEAP_BYTES / sizeof(*block); offset += PAGE_BYTES / sizeof(*block))
		block[offset] = offset + 1;
	bool contents = true;
	for (uint32_t offset = 0; offset < HEAP_BYTES / sizeof(*block); offset += PAGE_BYTES / sizeof(*block))
		contents &= block[offset] == offset + 1;
	const uintptr_t allocated_break = (uintptr_t)sbrk(0);
	const uint32_t before = available();
	free((void *)block);
	const uintptr_t freed_break = (uintptr_t)sbrk(0);
	const uint32_t after = available();
	xsdk_log_write("pages: heap breaks before=%08X allocated=%08X freed=%08X; pages=%u -> %u -> %u",
		(unsigned)original_break, (unsigned)allocated_break, (unsigned)freed_break, (unsigned)original_pages,
		(unsigned)before, (unsigned)after);
	check("pages: heap buffer stores data on all 8192 pages", contents);
	check("pages: freeing 32 MB at the heap top returns its pages",
		allocated_break != UINTPTR_MAX && freed_break != UINTPTR_MAX && allocated_break >= freed_break &&
			allocated_break - freed_break >= HEAP_BYTES && after >= before + HEAP_BYTES / PAGE_BYTES);
}

static void written_code(void)
{
	void *pool;
	size_t bytes;
	xsdk_jit_pool(&pool, &bytes);
	if (bytes < 2 * CACHE_LINE_BYTES || (uintptr_t)pool % 65536) {
		check("jit: the executable pool is 64 KB aligned", false);
		return;
	}
	uint32_t *code = pool;
	uint32_t (*function)(void) = (uint32_t (*)(void))pool;
	code[0] = 0x3860002A; // li r3,42
	code[1] = 0x4E800020; // blr
	xsdk_code_flush(code, 8);
	check("jit: written code returns 42", function() == 42);
	code[0] = 0x38600063; // li r3,99
	xsdk_code_flush(code, 8);
	check("jit: rewritten code returns 99", function() == 99);
	xsdk_code_flush(NULL, 0);
}

static void written_ranges(void)
{
	void *pool;
	size_t bytes;
	xsdk_jit_pool(&pool, &bytes);
	if (bytes < 2 * CACHE_LINE_BYTES)
		return;
	uint32_t *code = (uint32_t *)((unsigned char *)pool + CACHE_LINE_BYTES - sizeof(uint32_t));
	uint32_t (*function)(void) = (uint32_t (*)(void))code;
	code[0] = 0x38600029; // li r3,41
	code[1] = 0x38630001; // addi r3,r3,1
	code[2] = 0x4E800020; // blr
	xsdk_code_flush((unsigned char *)code + 1, 3 * sizeof(*code) - 1);
	check("jit: unaligned range crossing a line returns 42", function() == 42);
	code[0] = 0x38600061; // li r3,97
	code[1] = 0x38630002; // addi r3,r3,2
	xsdk_code_flush(pool, CACHE_LINE_BYTES + 1);
	check("jit: 129-byte range publishes both lines and returns 99", function() == 99);
}

void test_pages(void)
{
	mappings();
	refused();
	physical_protection();
	heap_return();
	written_code();
	written_ranges();
}
