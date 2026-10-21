#include <sys/mman.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"

enum { PAGE_BYTES = 4096 };

typedef struct Mapping {
	struct Mapping *next;
	uintptr_t base;
	uint32_t bytes;
	uint32_t committed_pages;
	unsigned char committed[];
} Mapping;

// Own only mmap's reservations so partial unmaps release each reservation after its last live page.
static Mapping *mappings;
static pthread_mutex_t mappings_lock = PTHREAD_MUTEX_INITIALIZER;

static int protection_bits(int protection)
{
	if (protection & PROT_EXEC) {
		errno = ENOTSUP;
		return -1;
	}
	switch (protection) {
	case PROT_NONE:
		return PAGE_NOACCESS;
	case PROT_READ:
		return PAGE_READONLY;
	case PROT_READ | PROT_WRITE:
		return PAGE_READWRITE;
	default:
		errno = EINVAL;
		return -1;
	}
}

static bool rounded_length(size_t length, uint32_t *bytes)
{
	if (!length || length > INT32_MAX - (PAGE_BYTES - 1)) {
		errno = EINVAL;
		return false;
	}
	*bytes = (length + PAGE_BYTES - 1) & ~(uint32_t)(PAGE_BYTES - 1);
	return true;
}

static int memory_error(const char *call, NTSTATUS status)
{
	xsdk_log_write("%s failed: 0x%08X", call, (unsigned)status);
	errno = ENOMEM;
	return -1;
}

void *mmap(void *address, size_t length, int protection, int flags, int descriptor, off_t offset)
{
	(void)address;
	if (flags != (MAP_PRIVATE | MAP_ANONYMOUS) || descriptor != -1 || offset != 0) {
		errno = ENOTSUP;
		return MAP_FAILED;
	}
	const int protect = protection_bits(protection);
	uint32_t bytes;
	if (protect < 0 || !rounded_length(length, &bytes))
		return MAP_FAILED;
	const int locked = pthread_mutex_lock(&mappings_lock);
	if (locked) {
		errno = locked;
		return MAP_FAILED;
	}
	const uint32_t requested_bytes = bytes;
	void *base = NULL;
	NTSTATUS status = NtAllocateVirtualMemory(&base, &bytes, MEM_RESERVE | MEM_COMMIT, (uint32_t)protect, 0);
	if (!NT_SUCCESS(status)) {
		(void)pthread_mutex_unlock(&mappings_lock);
		(void)memory_error("mmap: NtAllocateVirtualMemory", status);
		return MAP_FAILED;
	}
	const uint32_t pages = requested_bytes / PAGE_BYTES;
	Mapping *mapping = malloc(sizeof(*mapping) + (pages + 7) / 8);
	if (!mapping) {
		uint32_t release_bytes = 0;
		status = NtFreeVirtualMemory(&base, &release_bytes, MEM_RELEASE, 0);
		if (!NT_SUCCESS(status))
			(void)memory_error("mmap: NtFreeVirtualMemory(cleanup)", status);
		(void)pthread_mutex_unlock(&mappings_lock);
		errno = ENOMEM;
		return MAP_FAILED;
	}
	*mapping =
		(Mapping){ .next = mappings, .base = (uintptr_t)base, .bytes = requested_bytes, .committed_pages = pages };
	memset(mapping->committed, 0xFF, (pages + 7) / 8);
	mappings = mapping;
	(void)pthread_mutex_unlock(&mappings_lock);
	return base;
}

static bool page_range(void *address, size_t length, uintptr_t *end)
{
	uint32_t bytes;
	const uintptr_t base = (uintptr_t)address;
	if (base % PAGE_BYTES || !rounded_length(length, &bytes) || bytes > UINTPTR_MAX - base) {
		errno = EINVAL;
		return false;
	}
	*end = base + bytes;
	return true;
}

static int decommit(Mapping *mapping, uintptr_t lo, uintptr_t hi)
{
	uint32_t page = (lo - mapping->base) / PAGE_BYTES;
	const uint32_t end = (hi - mapping->base) / PAGE_BYTES;
	while (page < end) {
		if (!(mapping->committed[page / 8] & (1u << (page % 8)))) {
			page++;
			continue;
		}
		const uint32_t first = page;
		do {
			page++;
		} while (page < end && (mapping->committed[page / 8] & (1u << (page % 8))));
		void *base = (void *)(mapping->base + first * PAGE_BYTES);
		uint32_t bytes = (page - first) * PAGE_BYTES;
		const NTSTATUS status = NtFreeVirtualMemory(&base, &bytes, MEM_DECOMMIT, 0);
		if (!NT_SUCCESS(status))
			return memory_error("munmap: NtFreeVirtualMemory(decommit)", status);
		for (uint32_t i = first; i < page; i++)
			mapping->committed[i / 8] &= ~(1u << (i % 8));
		mapping->committed_pages -= page - first;
	}
	return 0;
}

int munmap(void *address, size_t length)
{
	uintptr_t end;
	if (!page_range(address, length, &end))
		return -1;
	const int locked = pthread_mutex_lock(&mappings_lock);
	if (locked) {
		errno = locked;
		return -1;
	}
	const uintptr_t start = (uintptr_t)address;
	int result = 0;
	Mapping **slot = &mappings;
	while (*slot) {
		Mapping *mapping = *slot;
		const uintptr_t lo = start > mapping->base ? start : mapping->base;
		const uintptr_t mapping_end = mapping->base + mapping->bytes;
		const uintptr_t hi = end < mapping_end ? end : mapping_end;
		if (lo >= hi) {
			slot = &mapping->next;
			continue;
		}
		result = decommit(mapping, lo, hi);
		if (result)
			break;
		if (mapping->committed_pages) {
			slot = &mapping->next;
			continue;
		}
		void *base = (void *)mapping->base;
		uint32_t bytes = 0;
		const NTSTATUS status = NtFreeVirtualMemory(&base, &bytes, MEM_RELEASE, 0);
		if (!NT_SUCCESS(status)) {
			result = memory_error("munmap: NtFreeVirtualMemory(release)", status);
			break;
		}
		*slot = mapping->next;
		free(mapping);
	}
	(void)pthread_mutex_unlock(&mappings_lock);
	return result;
}

int mprotect(void *address, size_t length, int protection)
{
	const int protect = protection_bits(protection);
	uintptr_t end;
	if (protect < 0 || !page_range(address, length, &end))
		return -1;
	const int locked = pthread_mutex_lock(&mappings_lock);
	if (locked) {
		errno = locked;
		return -1;
	}
	uint32_t bytes = end - (uintptr_t)address;
	uint32_t old;
	const NTSTATUS status = NtProtectVirtualMemory(&address, &bytes, (uint32_t)protect, &old, 0);
	(void)pthread_mutex_unlock(&mappings_lock);
	return NT_SUCCESS(status) ? 0 : memory_error("mprotect: NtProtectVirtualMemory", status);
}
