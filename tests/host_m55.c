// Kernel mocks exercise bookkeeping and failure recovery; they cannot establish console ABI or scheduling.
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "xsdk/kernel.h"

enum { PAGE_BYTES = 4096, REGIONS = 16, PAGES = 256 };

typedef struct {
	uintptr_t base;
	uint32_t bytes;
	bool committed[PAGES];
	uint32_t protection[PAGES];
} Region;

static Region regions[REGIONS];
static uint32_t allocations;
static uint32_t releases;
static NTSTATUS allocation_failure;
static NTSTATUS free_failure;
static NTSTATUS protect_failure;
static uintptr_t heap_base = 0x40000000;
static uint32_t heap_committed;
void *sbrk(ptrdiff_t increment);

NTSTATUS MmQueryStatistics(MM_QUERY_STATISTICS_RESULT *stats)
{
	stats->title.available_pages = (64 * 1024 * 1024 - heap_committed) / PAGE_BYTES;
	return STATUS_SUCCESS;
}

void xsdk_log_write(const char *format, ...)
{
	(void)format;
}

NTSTATUS NtAllocateVirtualMemory(void **base, uint32_t *bytes, uint32_t type, uint32_t protect, uint32_t debug)
{
	assert(!debug);
	if (type == MEM_RESERVE) {
		*base = (void *)heap_base;
		return STATUS_SUCCESS;
	}
	if (type == MEM_COMMIT) {
		assert((uintptr_t)*base == heap_base + heap_committed);
		heap_committed += *bytes;
		return STATUS_SUCCESS;
	}
	assert(type == (MEM_RESERVE | MEM_COMMIT) && *bytes <= PAGES * PAGE_BYTES);
	if (allocation_failure)
		return allocation_failure;
	for (uint32_t i = 0; i < REGIONS; i++) {
		Region *region = &regions[i];
		if (region->base)
			continue;
		region->base = 0x1000000 + i * 0x100000;
		region->bytes = *bytes;
		for (uint32_t page = 0; page < *bytes / PAGE_BYTES; page++) {
			region->committed[page] = true;
			region->protection[page] = protect;
		}
		*base = (void *)region->base;
		allocations++;
		return STATUS_SUCCESS;
	}
	return STATUS_NO_MEMORY;
}

static Region *region_at(uintptr_t address)
{
	for (uint32_t i = 0; i < REGIONS; i++)
		if (regions[i].base && address >= regions[i].base && address - regions[i].base < regions[i].bytes)
			return &regions[i];
	return NULL;
}

NTSTATUS NtFreeVirtualMemory(void **base, uint32_t *bytes, uint32_t type, uint32_t debug)
{
	assert(!debug);
	if (free_failure)
		return free_failure;
	if ((uintptr_t)*base >= heap_base) {
		assert(type == MEM_DECOMMIT && (uintptr_t)*base + *bytes == heap_base + heap_committed);
		heap_committed -= *bytes;
		return STATUS_SUCCESS;
	}
	Region *region = region_at((uintptr_t)*base);
	assert(region);
	if (type == MEM_RELEASE) {
		assert((uintptr_t)*base == region->base && *bytes == 0);
		memset(region, 0, sizeof(*region));
		releases++;
	} else {
		assert(type == MEM_DECOMMIT && (uintptr_t)*base + *bytes <= region->base + region->bytes);
		for (uint32_t page = ((uintptr_t)*base - region->base) / PAGE_BYTES;
			page < ((uintptr_t)*base - region->base + *bytes) / PAGE_BYTES; page++)
			region->committed[page] = false;
	}
	return STATUS_SUCCESS;
}

NTSTATUS NtProtectVirtualMemory(void **base, uint32_t *bytes, uint32_t protect, uint32_t *old, uint32_t debug)
{
	assert(!debug);
	if (protect_failure)
		return protect_failure;
	Region *region = region_at((uintptr_t)*base);
	if (!region || (uintptr_t)*base + *bytes > region->base + region->bytes)
		return STATUS_NO_MEMORY;
	for (uint32_t page = ((uintptr_t)*base - region->base) / PAGE_BYTES;
		page < ((uintptr_t)*base - region->base + *bytes) / PAGE_BYTES; page++)
		if (!region->committed[page])
			return STATUS_NO_MEMORY;
	*old = region->protection[((uintptr_t)*base - region->base) / PAGE_BYTES];
	for (uint32_t page = ((uintptr_t)*base - region->base) / PAGE_BYTES;
		page < ((uintptr_t)*base - region->base + *bytes) / PAGE_BYTES; page++) {
		assert(region->committed[page]);
		region->protection[page] = protect;
	}
	return STATUS_SUCCESS;
}

static void pages(void)
{
	const int flags = MAP_PRIVATE | MAP_ANONYMOUS;
	allocation_failure = STATUS_NO_MEMORY;
	assert(mmap(NULL, PAGE_BYTES, PROT_READ, flags, -1, 0) == MAP_FAILED && errno == ENOMEM);
	allocation_failure = 0;
	unsigned char *base = mmap(NULL, 3 * PAGE_BYTES - 1, PROT_READ | PROT_WRITE, flags, -1, 0);
	assert(base != MAP_FAILED);
	assert(region_at((uintptr_t)base)->bytes == 3 * PAGE_BYTES);
	assert(mprotect(base + PAGE_BYTES, 1, PROT_READ) == 0);
	assert(region_at((uintptr_t)base)->protection[1] == PAGE_READONLY);
	assert(mprotect(base, 3 * PAGE_BYTES, PROT_NONE) == 0);
	assert(mprotect(base, 3 * PAGE_BYTES, PROT_READ | PROT_WRITE) == 0);
	protect_failure = STATUS_NO_MEMORY;
	assert(mprotect(base, PAGE_BYTES, PROT_READ) == -1 && errno == ENOMEM);
	protect_failure = 0;
	free_failure = STATUS_NO_MEMORY;
	assert(munmap(base + PAGE_BYTES, PAGE_BYTES) == -1);
	assert(mprotect(base + PAGE_BYTES, PAGE_BYTES, PROT_READ) == 0);
	free_failure = 0;
	assert(munmap(base + PAGE_BYTES, 1) == 0);
	assert(munmap(base + PAGE_BYTES, 1) == 0);
	assert(mprotect(base, 3 * PAGE_BYTES, PROT_READ) == -1 && errno == ENOMEM);
	assert(region_at((uintptr_t)base)->protection[0] == PAGE_READWRITE);
	assert(munmap(base, PAGE_BYTES) == 0 && releases == 0);
	assert(munmap(base + 2 * PAGE_BYTES, PAGE_BYTES) == 0 && releases == 1);
	assert(munmap(base, 3 * PAGE_BYTES) == 0 && releases == 1);
	assert(mmap(NULL, PAGE_BYTES, PROT_EXEC, flags, -1, 0) == MAP_FAILED && errno == ENOTSUP);
	assert(mmap(NULL, PAGE_BYTES, PROT_READ, flags | MAP_FIXED, -1, 0) == MAP_FAILED && errno == ENOTSUP);
	assert(mmap(NULL, PAGE_BYTES, PROT_READ, MAP_PRIVATE, 0, 0) == MAP_FAILED && errno == ENOTSUP);
	assert(mmap(NULL, SIZE_MAX, PROT_READ, flags, -1, 0) == MAP_FAILED && errno == EINVAL);
	assert(munmap((void *)(UINTPTR_MAX - 4095), PAGE_BYTES) == -1 && errno == EINVAL);
	assert(munmap((void *)1, PAGE_BYTES) == -1 && errno == EINVAL);
	assert(mprotect(NULL, PAGE_BYTES, PROT_READ) == -1 && errno == ENOMEM);
	assert(allocations == releases);
	puts("PASS: mmap partial unmap, protection, holes, validation, and kernel failures");
}

typedef struct {
	bool open;
	int32_t value;
	int32_t limit;
} KernelSemaphore;

static KernelSemaphore semaphores[16];
static pthread_mutex_t kernel_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t kernel_changed = PTHREAD_COND_INITIALIZER;
static bool release_failure;

NTSTATUS NtCreateSemaphore(HANDLE *handle, const OBJECT_ATTRIBUTES *attributes, int32_t count, int32_t limit)
{
	(void)attributes;
	pthread_mutex_lock(&kernel_lock);
	for (uint32_t i = 1; i < 16; i++) {
		if (!semaphores[i].open) {
			semaphores[i] = (KernelSemaphore){ true, count, limit };
			*handle = i;
			pthread_mutex_unlock(&kernel_lock);
			return STATUS_SUCCESS;
		}
	}
	pthread_mutex_unlock(&kernel_lock);
	return STATUS_NO_MEMORY;
}

NTSTATUS NtClose(HANDLE handle)
{
	pthread_mutex_lock(&kernel_lock);
	const NTSTATUS status = handle < 16 && semaphores[handle].open ? STATUS_SUCCESS : STATUS_INVALID_HANDLE;
	if (status == STATUS_SUCCESS)
		semaphores[handle].open = false;
	pthread_mutex_unlock(&kernel_lock);
	return status;
}

NTSTATUS NtReleaseSemaphore(HANDLE handle, int32_t count, int32_t *previous)
{
	if (release_failure)
		return STATUS_INVALID_HANDLE;
	assert(count == 1);
	pthread_mutex_lock(&kernel_lock);
	assert(handle < 16 && semaphores[handle].open && semaphores[handle].value < semaphores[handle].limit);
	if (previous)
		*previous = semaphores[handle].value;
	semaphores[handle].value++;
	pthread_cond_signal(&kernel_changed);
	pthread_mutex_unlock(&kernel_lock);
	return STATUS_SUCCESS;
}

NTSTATUS NtWaitForSingleObjectEx(HANDLE handle, uint32_t mode, uint32_t alertable, const int64_t *timeout)
{
	assert(mode == USER_MODE && !alertable);
	pthread_mutex_lock(&kernel_lock);
	assert(handle < 16 && semaphores[handle].open);
	struct timespec until;
	clock_gettime(CLOCK_REALTIME, &until);
	if (timeout) {
		assert(*timeout <= 0);
		const int64_t ns = -(*timeout % 10000000) * 100 + until.tv_nsec;
		until.tv_sec += -*timeout / 10000000 + ns / 1000000000;
		until.tv_nsec = ns % 1000000000;
	}
	while (!semaphores[handle].value) {
		const int result = timeout ? pthread_cond_timedwait(&kernel_changed, &kernel_lock, &until)
								   : pthread_cond_wait(&kernel_changed, &kernel_lock);
		if (result == ETIMEDOUT) {
			pthread_mutex_unlock(&kernel_lock);
			return STATUS_TIMEOUT;
		}
		assert(!result);
	}
	semaphores[handle].value--;
	pthread_mutex_unlock(&kernel_lock);
	return STATUS_SUCCESS;
}

static void *post_many(void *context)
{
	for (uint32_t i = 0; i < 10000; i++)
		assert(sem_post(context) == 0);
	return NULL;
}

static void *take_many(void *context)
{
	for (uint32_t i = 0; i < 10000; i++)
		assert(sem_wait(context) == 0);
	return NULL;
}

static void semaphore_checks(void)
{
	sem_t sem;
	assert(sem_init(&sem, 0, 0) == 0);
	assert(sem_trywait(&sem) == -1 && errno == EAGAIN);
	pthread_t workers[4];
	for (uint32_t i = 0; i < 4; i++)
		assert(pthread_create(&workers[i], NULL, i % 2 ? post_many : take_many, &sem) == 0);
	for (uint32_t i = 0; i < 4; i++)
		assert(pthread_join(workers[i], NULL) == 0);
	int value;
	assert(sem_getvalue(&sem, &value) == 0 && value == 0);
	release_failure = true;
	assert(sem_post(&sem) == -1 && errno == EINVAL);
	assert(sem_getvalue(&sem, &value) == 0 && value == 0);
	release_failure = false;
	struct timespec until;
	clock_gettime(CLOCK_MONOTONIC, &until);
	until.tv_nsec += 20000000;
	until.tv_sec += until.tv_nsec / 1000000000;
	until.tv_nsec %= 1000000000;
	assert(sem_clockwait(&sem, CLOCK_MONOTONIC, &until) == -1 && errno == ETIMEDOUT);
	struct timespec after;
	clock_gettime(CLOCK_MONOTONIC, &after);
	assert(after.tv_sec > until.tv_sec || (after.tv_sec == until.tv_sec && after.tv_nsec >= until.tv_nsec));
	const struct timespec past = { 0 };
	assert(sem_post(&sem) == 0 && sem_timedwait(&sem, &past) == 0);
	assert(sem_destroy(&sem) == 0);
	assert(sem_init(&sem, 0, SEM_VALUE_MAX) == 0);
	assert(sem_post(&sem) == -1 && errno == EOVERFLOW);
	assert(sem_getvalue(&sem, &value) == 0 && value == SEM_VALUE_MAX);
	assert(sem_destroy(&sem) == 0);
	assert(sem_init(&sem, 1, 0) == -1 && errno == ENOTSUP);
	puts("PASS: semaphores 20000 contended posts/takes, deadlines, overflow, and failed-post rollback");
}

static void heap_checks(void)
{
	assert((uintptr_t)sbrk(0) == heap_base);
	assert((uintptr_t)sbrk(131073) == heap_base && heap_committed == 196608);
	assert((uintptr_t)sbrk(-1) == heap_base + 131073 && heap_committed == 131072);
	free_failure = STATUS_NO_MEMORY;
	assert(sbrk(-65536) == (void *)-1 && errno == ENOMEM);
	assert((uintptr_t)sbrk(0) == heap_base + 131072 && heap_committed == 131072);
	free_failure = 0;
	assert((uintptr_t)sbrk(-65536) == heap_base + 131072 && heap_committed == 65536);
	assert((uintptr_t)sbrk(1) == heap_base + 65536 && heap_committed == 131072);
	assert(sbrk(PTRDIFF_MIN) == (void *)-1 && errno == EINVAL);
	assert((uintptr_t)sbrk(-65537) == heap_base + 65537 && heap_committed == 0);
	puts("PASS: heap trim rounds to 64 KB, recommits on growth, and preserves the break on failure");
}

int main(void)
{
	pages();
	semaphore_checks();
	heap_checks();
	return 0;
}
