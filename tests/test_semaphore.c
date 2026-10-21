#include "tests.h"

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <time.h>

#include "xsdk/log.h"
#include "xsdk/thread.h"

static struct timespec deadline(clockid_t clock, long ms)
{
	struct timespec result;
	(void)clock_gettime(clock, &result);
	result.tv_nsec += ms * 1000000;
	result.tv_sec += result.tv_nsec / 1000000000;
	result.tv_nsec %= 1000000000;
	return result;
}

static void *consume(void *context)
{
	sem_t *semaphore = context;
	return (void *)(intptr_t)sem_wait(semaphore);
}

static bool times_out(sem_t *semaphore, clockid_t clock)
{
	const struct timespec until = deadline(clock, 20);
	const int result =
		clock == CLOCK_REALTIME ? sem_timedwait(semaphore, &until) : sem_clockwait(semaphore, clock, &until);
	const int error = errno;
	struct timespec after;
	(void)clock_gettime(clock, &after);
	const int64_t late_ns = (int64_t)(after.tv_sec - until.tv_sec) * 1000000000 + after.tv_nsec - until.tv_nsec;
	return result == -1 && error == ETIMEDOUT && late_ns >= 0 && late_ns < 20000000;
}

void test_semaphore(void)
{
	sem_t semaphore;
	if (sem_init(&semaphore, 0, 0) != 0) {
		check("semaphore: initialize", false);
		return;
	}
	pthread_t thread;
	if (pthread_create(&thread, NULL, consume, &semaphore) != 0)
		exit(1);
	if (!NT_SUCCESS(xsdk_thread_sleep(20)))
		exit(1);
	const int64_t poll = 0;
	const NTSTATUS blocked = NtWaitForSingleObjectEx(xsdk_pthread_handle(thread), USER_MODE, 0, &poll);
	const int posted = sem_post(&semaphore);
	const int64_t timeout = -20000000;
	if (NtWaitForSingleObjectEx(xsdk_pthread_handle(thread), USER_MODE, 0, &timeout) != STATUS_SUCCESS)
		exit(1);
	void *result = (void *)-1;
	const int joined = pthread_join(thread, &result);
	check("semaphore: post wakes a worker in sem_wait",
		blocked == STATUS_TIMEOUT && posted == 0 && joined == 0 && !result);
	check("semaphore: trywait at zero returns EAGAIN", sem_trywait(&semaphore) == -1 && errno == EAGAIN);
	check("semaphore: realtime timedwait expires at its deadline", times_out(&semaphore, CLOCK_REALTIME));
	check("semaphore: monotonic clockwait expires at its deadline", times_out(&semaphore, CLOCK_MONOTONIC));
	const struct timespec invalid = { .tv_sec = 0, .tv_nsec = 1000000000 };
	check("semaphore: invalid clock and nanoseconds are refused",
		sem_clockwait(&semaphore, (clockid_t)-1, &invalid) == -1 && errno == EINVAL &&
			sem_timedwait(&semaphore, &invalid) == -1 && errno == EINVAL);
	int value = -1;
	bool counted =
		sem_post(&semaphore) == 0 && sem_post(&semaphore) == 0 && sem_getvalue(&semaphore, &value) == 0 && value == 2;
	counted &= sem_trywait(&semaphore) == 0 && sem_getvalue(&semaphore, &value) == 0 && value == 1;
	check("semaphore: getvalue counts posts and successful takes", counted);
	const struct timespec past = { 0 };
	check("semaphore: an available token succeeds even past its deadline", sem_timedwait(&semaphore, &past) == 0);
	const HANDLE closed = semaphore.handle;
	check("semaphore: destroy releases its handle",
		sem_destroy(&semaphore) == 0 && NtWaitForSingleObjectEx(closed, USER_MODE, 0, &poll) == STATUS_INVALID_HANDLE);
	check("semaphore: shared and excessive initial counts are refused",
		sem_init(&semaphore, 1, 0) == -1 && errno == ENOTSUP &&
			sem_init(&semaphore, 0, (unsigned)SEM_VALUE_MAX + 1) == -1 && errno == EINVAL);
	if (sem_init(&semaphore, 0, SEM_VALUE_MAX) != 0) {
		check("semaphore: initialize maximum count", false);
		return;
	}
	check("semaphore: post at the limit returns EOVERFLOW without changing count",
		sem_post(&semaphore) == -1 && errno == EOVERFLOW && sem_getvalue(&semaphore, &value) == 0 &&
			value == SEM_VALUE_MAX);
	(void)sem_destroy(&semaphore);
}
