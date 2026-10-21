#include <semaphore.h>

#include <errno.h>
#include <stdint.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"

int sem_init(sem_t *semaphore, int shared, unsigned value)
{
	if (shared || value > SEM_VALUE_MAX) {
		errno = shared ? ENOTSUP : EINVAL;
		return -1;
	}
	HANDLE handle;
	const NTSTATUS status = NtCreateSemaphore(&handle, NULL, (int32_t)value, SEM_VALUE_MAX);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("sem_init: NtCreateSemaphore failed: 0x%08X", (unsigned)status);
		errno = ENOMEM;
		return -1;
	}
	*semaphore = (sem_t){ .handle = handle, .value = (int32_t)value };
	return 0;
}

int sem_destroy(sem_t *semaphore)
{
	const NTSTATUS status = NtClose(semaphore->handle);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("sem_destroy: NtClose failed: 0x%08X", (unsigned)status);
		errno = EINVAL;
		return -1;
	}
	semaphore->handle = 0;
	return 0;
}

static int take(sem_t *semaphore, const int64_t *timeout)
{
	const NTSTATUS status = NtWaitForSingleObjectEx(semaphore->handle, USER_MODE, 0, timeout);
	if (status == STATUS_SUCCESS) {
		__atomic_fetch_sub(&semaphore->value, 1, __ATOMIC_ACQUIRE);
		return 0;
	}
	if (status == STATUS_TIMEOUT) {
		errno = EAGAIN;
		return -1;
	}
	xsdk_log_write("sem_wait: NtWaitForSingleObjectEx failed: 0x%08X", (unsigned)status);
	errno = EINVAL;
	return -1;
}

int sem_wait(sem_t *semaphore)
{
	return take(semaphore, NULL);
}

int sem_trywait(sem_t *semaphore)
{
	const int64_t timeout = 0;
	return take(semaphore, &timeout);
}

int sem_clockwait(sem_t *semaphore, clockid_t clock, const struct timespec *deadline)
{
	if ((clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC) || deadline->tv_nsec < 0 ||
		deadline->tv_nsec >= 1000000000) {
		errno = EINVAL;
		return -1;
	}
	if (sem_trywait(semaphore) == 0)
		return 0;
	if (errno != EAGAIN)
		return -1;
	for (;;) {
		struct timespec now;
		if (clock_gettime(clock, &now) != 0)
			return -1;
		if (deadline->tv_sec < now.tv_sec || (deadline->tv_sec == now.tv_sec && deadline->tv_nsec <= now.tv_nsec)) {
			errno = ETIMEDOUT;
			return -1;
		}
		const uint64_t seconds = (uint64_t)deadline->tv_sec - (uint64_t)now.tv_sec;
		// Bound the conversion even for a deadline near time_t's maximum.
		const int64_t ns =
			seconds > 86400 ? 86400000000000LL : (int64_t)seconds * 1000000000 + deadline->tv_nsec - now.tv_nsec;
		const int64_t timeout = -((ns + 99) / 100);
		if (take(semaphore, &timeout) == 0)
			return 0;
		if (errno != EAGAIN)
			return -1;
		// A kernel timeout may arrive a tick early (docs/HARDWARE.md, time base).
	}
}

int sem_timedwait(sem_t *semaphore, const struct timespec *deadline)
{
	return sem_clockwait(semaphore, CLOCK_REALTIME, deadline);
}

int sem_post(sem_t *semaphore)
{
	int32_t value = __atomic_load_n(&semaphore->value, __ATOMIC_RELAXED);
	do {
		if (value == SEM_VALUE_MAX) {
			errno = EOVERFLOW;
			return -1;
		}
	} while (
		!__atomic_compare_exchange_n(&semaphore->value, &value, value + 1, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
	// Publish the count before waking a waiter, so its decrement cannot precede this increment.
	const NTSTATUS status = NtReleaseSemaphore(semaphore->handle, 1, NULL);
	if (!NT_SUCCESS(status)) {
		__atomic_fetch_sub(&semaphore->value, 1, __ATOMIC_RELAXED);
		xsdk_log_write("sem_post: NtReleaseSemaphore failed: 0x%08X", (unsigned)status);
		errno = EINVAL;
		return -1;
	}
	return 0;
}

int sem_getvalue(sem_t *semaphore, int *value)
{
	*value = __atomic_load_n(&semaphore->value, __ATOMIC_RELAXED);
	return 0;
}
