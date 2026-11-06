#include <errno.h>
#include <malloc.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tests.h"

#include "xsdk/kernel.h"
#include "xsdk/log.h"

// MmQueryStatistics counts 4 KB pages (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc:496).
enum {
	PAGE_BYTES = 4096,
	GROWTH_BYTES = 16 * 1024 * 1024,
	LARGE_PAGE_BYTES = 64 * 1024,
	ALIGNED_BYTES = 100,
	// Not a power of two, and a power of two below a pointer's size.
	ODD_ALIGNMENT = 24,
	SHORT_ALIGNMENT = 2,
	// newlib maps blocks of 256 KB and more apart from its heap (newlib-xsdk.patch).
	MAP_THRESHOLD_BYTES = 256 * 1024,
	SMALL_BYTES = 64 * 1024,
	LARGE_BYTES = 1024 * 1024,
	ALIGNED_LARGE_BYTES = 512 * 1024,
	PIN_BYTES = 4096,
	// A page table and the heap's 64 KB commit for the pin are all a freed large block may leave taken.
	PAGE_MARGIN = 32,
	ROUNDS = 16,
	// 16 pins, each at most two pages of heap; kept in the heap, the large blocks would grow it by 11.5 MB.
	ROUNDS_GROWTH_BYTES = 256 * 1024,
};

// Memory at each alignment, written to and freed; the alignments POSIX refuses, refused.
static bool aligned_allocations(void)
{
	static const size_t alignments[] = { sizeof(void *), 64, PAGE_BYTES, LARGE_PAGE_BYTES };
	bool all = true;
	for (size_t i = 0; i < sizeof(alignments) / sizeof(alignments[0]); i++) {
		void *memory = NULL;
		const bool aligned = posix_memalign(&memory, alignments[i], ALIGNED_BYTES) == 0 && memory &&
			((uintptr_t)memory & (alignments[i] - 1)) == 0;
		if (!aligned)
			xsdk_log_write(
				"memory: posix_memalign(%u) gave 0x%08X", (unsigned)alignments[i], (unsigned)(uintptr_t)memory);
		if (memory)
			memset(memory, 0xA5, ALIGNED_BYTES);
		free(memory);
		all &= aligned;
	}
	void *memory = NULL;
	return all && posix_memalign(&memory, ODD_ALIGNMENT, ALIGNED_BYTES) == EINVAL &&
		posix_memalign(&memory, SHORT_ALIGNMENT, ALIGNED_BYTES) == EINVAL;
}

static NTSTATUS query(MM_QUERY_STATISTICS_RESULT *statistics, uint32_t size_bytes)
{
	*statistics = (MM_QUERY_STATISTICS_RESULT){ .size = size_bytes };
	return MmQueryStatistics(statistics);
}

static void log_section(const char *name, const MM_QUERY_STATISTICS_SECTION *section)
{
	xsdk_log_write("memory: %s: %u pages free; %u MB virtual, %u MB reserved", name, (unsigned)section->available_pages,
		(unsigned)(section->total_virtual_memory_bytes >> 20),
		(unsigned)(section->reserved_virtual_memory_bytes >> 20));
	xsdk_log_write("memory: %s pages: %u physical, %u pool, %u stack, %u image, %u heap", name,
		(unsigned)section->physical_pages, (unsigned)section->pool_pages, (unsigned)section->stack_pages,
		(unsigned)section->image_pages, (unsigned)section->heap_pages);
	xsdk_log_write("memory: %s pages: %u virtual, %u page table, %u cache", name, (unsigned)section->virtual_pages,
		(unsigned)section->page_table_pages, (unsigned)section->cache_pages);
}

static void log_statistics(const MM_QUERY_STATISTICS_RESULT *statistics)
{
	xsdk_log_write("memory: %u pages in all, %u the kernel's, highest page 0x%X",
		(unsigned)statistics->total_physical_pages, (unsigned)statistics->kernel_pages,
		(unsigned)statistics->highest_physical_page);
	log_section("title", &statistics->title);
	log_section("system", &statistics->system);
	unsigned free_mb = statistics->title.available_pages / (1048576 / PAGE_BYTES);
	xsdk_log_write("memory: %u MB free for the title", free_mb);
}

// Grows the heap, touching every page, and returns the free pages the kernel then reports.
static uint32_t available_after_growth(void)
{
	volatile uint8_t *grown = sbrk(GROWTH_BYTES);
	if (grown == (void *)-1) {
		xsdk_log_write("memory: sbrk(%u) failed", (unsigned)GROWTH_BYTES);
		return 0;
	}
	for (size_t offset = 0; offset < GROWTH_BYTES; offset += PAGE_BYTES)
		grown[offset] = 1;
	MM_QUERY_STATISTICS_RESULT statistics;
	NTSTATUS status = query(&statistics, sizeof(statistics));
	(void)sbrk(-GROWTH_BYTES);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("memory: MmQueryStatistics failed: 0x%08X", (unsigned)status);
		return 0;
	}
	return statistics.title.available_pages;
}

static uint32_t free_pages(void)
{
	MM_QUERY_STATISTICS_RESULT statistics;
	return NT_SUCCESS(query(&statistics, sizeof(statistics))) ? statistics.title.available_pages : 0;
}

static bool in_sbrk_heap(const volatile void *block, size_t size_bytes)
{
	const uintptr_t heap_end = (uintptr_t)sbrk(0);
	return (uintptr_t)block < heap_end && (uintptr_t)block + size_bytes > heap_end - mallinfo().arena;
}

// Freed under a small block, a large one in the heap would leave its pages committed below it.
static void large_block(void)
{
	const struct mallinfo before = mallinfo();
	const uint32_t pages_before = free_pages();
	volatile uint8_t *large = malloc(LARGE_BYTES);
	void *pin = malloc(PIN_BYTES);
	if (!large || !pin) {
		check("malloc: 1 MB and 4 KB", false);
		free((void *)large);
		free(pin);
		return;
	}
	large[0] = 1;
	large[LARGE_BYTES - 1] = 1;
	const struct mallinfo mapped = mallinfo();
	const bool apart = !in_sbrk_heap(large, LARGE_BYTES);
	const uintptr_t address = (uintptr_t)large;
	free((void *)large);
	const uint32_t pages_after = free_pages();
	const size_t maps_after = mallinfo().hblks;
	free(pin);
	xsdk_log_write("memory: 1 MB at 0x%08X in %u new maps of %u KB; %u pages free before it, %u after its free",
		(unsigned)address, (unsigned)(mapped.hblks - before.hblks), (unsigned)((mapped.hblkhd - before.hblkhd) >> 10),
		(unsigned)pages_before, (unsigned)pages_after);
	check("malloc: 1 MB lands in a map of its own, outside the sbrk heap",
		apart && mapped.hblks == before.hblks + 1 && mapped.hblkhd - before.hblkhd >= LARGE_BYTES);
	check("free: 1 MB freed under a small block gives its pages back",
		maps_after == before.hblks && pages_after + PAGE_MARGIN >= pages_before);
}

static void fill(volatile uint32_t *words, size_t from_bytes, size_t to_bytes)
{
	for (size_t i = from_bytes / sizeof(*words); i < to_bytes / sizeof(*words); i++)
		words[i] = i + 1;
}

static bool filled(const volatile uint32_t *words, size_t size_bytes)
{
	bool same = true;
	for (size_t i = 0; i < size_bytes / sizeof(*words); i++)
		same &= words[i] == i + 1;
	return same;
}

// Frees the block if realloc fails, which leaves it.
static volatile uint32_t *resized(volatile uint32_t *words, size_t size_bytes)
{
	volatile uint32_t *moved = realloc((void *)words, size_bytes);
	if (!moved)
		free((void *)words);
	return moved;
}

// Grown past the threshold, a block moves into a map, and from it into a larger one; shrunk, it stays.
static void realloc_across(void)
{
	const char *name = "realloc: 64 KB grown to 1 MB and 2 MB moves into maps, and back to 64 KB, keeping its contents";
	const size_t maps = mallinfo().hblks;
	volatile uint32_t *words = malloc(SMALL_BYTES);
	if (!words)
		goto fail;
	fill(words, 0, SMALL_BYTES);
	words = resized(words, LARGE_BYTES);
	if (!words)
		goto fail;
	bool kept = filled(words, SMALL_BYTES) && mallinfo().hblks == maps + 1;
	fill(words, SMALL_BYTES, LARGE_BYTES);
	words = resized(words, 2 * LARGE_BYTES);
	if (!words)
		goto fail;
	kept &= filled(words, LARGE_BYTES) && mallinfo().hblks == maps + 1;
	words = resized(words, SMALL_BYTES);
	if (!words)
		goto fail;
	kept &= filled(words, SMALL_BYTES);
	free((void *)words);
	check(name, kept && mallinfo().hblks == maps);
	return;
fail:
	check(name, false);
}

// memalign hands out a map past its start; free must still release the whole map.
static void aligned_large_block(void)
{
	const size_t maps = mallinfo().hblks;
	const uint32_t pages_before = free_pages();
	volatile uint8_t *aligned = memalign(PAGE_BYTES, ALIGNED_LARGE_BYTES);
	void *pin = malloc(PIN_BYTES);
	if (!aligned || !pin) {
		check("memalign: 512 KB at 4 KB alignment, and 4 KB", false);
		free((void *)aligned);
		free(pin);
		return;
	}
	aligned[0] = 1;
	aligned[ALIGNED_LARGE_BYTES - 1] = 1;
	const uintptr_t address = (uintptr_t)aligned;
	const bool mapped = (address & (PAGE_BYTES - 1)) == 0 && mallinfo().hblks == maps + 1;
	free((void *)aligned);
	const uint32_t pages_after = free_pages();
	const bool released = mallinfo().hblks == maps;
	free(pin);
	xsdk_log_write("memory: memalign gave 0x%08X; %u pages free before it, %u after its free", (unsigned)address,
		(unsigned)pages_before, (unsigned)pages_after);
	check("memalign: 512 KB at 4 KB alignment is mapped and aligned, and its free gives its pages back",
		mapped && released && pages_after + PAGE_MARGIN >= pages_before);
}

// calloc leaves a map as the kernel commits it, so the kernel must zero it, pages a freed block dirtied too.
static void zeroed_large_block(void)
{
	const size_t maps = mallinfo().hblks;
	volatile uint32_t *dirty = malloc(LARGE_BYTES);
	if (!dirty) {
		check("malloc: 1 MB to dirty", false);
		return;
	}
	for (size_t i = 0; i < LARGE_BYTES / sizeof(*dirty); i++)
		dirty[i] = 0xA5A5A5A5;
	free((void *)dirty);
	volatile uint32_t *words = calloc(LARGE_BYTES / sizeof(*words), sizeof(*words));
	if (!words) {
		check("calloc: 1 MB", false);
		return;
	}
	bool zero = true;
	for (size_t i = 0; i < LARGE_BYTES / sizeof(*words); i++)
		zero &= words[i] == 0;
	const bool mapped = mallinfo().hblks == maps + 1;
	free((void *)words);
	check("calloc: 1 MB is mapped and reads zero after a freed 1 MB was dirtied", mapped && zero);
}

// Large blocks between small long-lived ones, in the heap, would leave holes no later, larger block fits.
static void fragmentation_round_trip(void)
{
	volatile uint8_t *large[ROUNDS] = { 0 };
	void *pins[ROUNDS] = { 0 };
	const size_t arena_before = mallinfo().arena;
	size_t arena_most = arena_before;
	bool allocated = true;
	for (uint32_t round = 0; round < ROUNDS; round++) {
		large[round] = malloc(MAP_THRESHOLD_BYTES + round * SMALL_BYTES);
		pins[round] = malloc(PIN_BYTES);
		allocated &= large[round] && pins[round];
		if (large[round])
			large[round][0] = 1;
		if (mallinfo().arena > arena_most)
			arena_most = mallinfo().arena;
	}
	for (uint32_t round = 0; round < ROUNDS; round++)
		free((void *)large[round]);
	const size_t arena_freed = mallinfo().arena;
	for (uint32_t round = 0; round < ROUNDS; round++)
		free(pins[round]);
	xsdk_log_write("memory: %u rounds grew the sbrk heap by %u KB at most, %u KB with only the small blocks left",
		(unsigned)ROUNDS, (unsigned)((arena_most - arena_before) >> 10),
		(unsigned)((arena_freed - arena_before) >> 10));
	check("malloc: 16 large blocks of 256 KB to 1.2 MB between small ones grow the sbrk heap by 256 KB at most",
		allocated && arena_most - arena_before <= ROUNDS_GROWTH_BYTES);
}

void test_memory(void)
{
	MM_QUERY_STATISTICS_RESULT statistics;
	check("MmQueryStatistics: a wrong size refused", !NT_SUCCESS(query(&statistics, sizeof(statistics) - 4)));

	NTSTATUS status = query(&statistics, sizeof(statistics));
	check("MmQueryStatistics: answers", NT_SUCCESS(status) && statistics.size == sizeof(statistics));
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("memory: MmQueryStatistics failed: 0x%08X", (unsigned)status);
		return;
	}
	log_statistics(&statistics);
	check("MmQueryStatistics: the title's free pages lie within the total",
		statistics.title.available_pages > 0 && statistics.title.available_pages <= statistics.total_physical_pages);
	check("sysconf: the physical pages and their size are the console's",
		sysconf(_SC_PAGESIZE) == PAGE_BYTES && sysconf(_SC_PHYS_PAGES) == (long)statistics.total_physical_pages);
	check("getpagesize: the page size sysconf gives", getpagesize() == sysconf(_SC_PAGESIZE));
	// sbrk reserved the title's free memory at the heap's first growth, before main, and free memory has only shrunk.
	check("sbrk: the heap's reservation covers the title's free memory",
		statistics.title.reserved_virtual_memory_bytes >= statistics.title.available_pages * PAGE_BYTES);

	uint32_t before = statistics.title.available_pages;
	uint32_t after = available_after_growth();
	xsdk_log_write("memory: %u pages free before %u MB of heap growth, %u after", (unsigned)before,
		(unsigned)(GROWTH_BYTES >> 20), (unsigned)after);
	check("MmQueryStatistics: heap growth takes free pages",
		after != 0 && after < before && before - after >= GROWTH_BYTES / PAGE_BYTES);
	check("posix_memalign: memory at alignments up to 64 KB, and EINVAL for 24 and 2", aligned_allocations());
	large_block();
	realloc_across();
	aligned_large_block();
	zeroed_large_block();
	fragmentation_round_trip();
}
