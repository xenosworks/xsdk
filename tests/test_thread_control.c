#include "tests.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <time.h>

#include "xsdk/log.h"
#include "xsdk/thread.h"

enum { WORKER_STACK = 64 * 1024 };

static int64_t now_ns(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static void finish(HANDLE thread)
{
	const int64_t timeout = -20000000;
	if (NtWaitForSingleObjectEx(thread, USER_MODE, 0, &timeout) != STATUS_SUCCESS) {
		check("thread control: worker finishes", false);
		exit(1);
	}
	(void)NtClose(thread);
}

static void priorities(void)
{
	int policy = -1;
	struct sched_param original = { .sched_priority = INT_MIN };
	const pthread_t self = pthread_self();
	const int queried = self ? pthread_getschedparam(self, &policy, &original) : ESRCH;
	xsdk_log_write("thread control: original query=%d policy=%d priority=%d", queried, policy, original.sched_priority);
	if (queried) {
		check("thread control: priority query", false);
		return;
	}
	enum { LEVELS = XSDK_PRIORITY_MAX - XSDK_PRIORITY_MIN + 1 };
	struct {
		int set;
		int get;
		int actual;
		int policy;
	} samples[LEVELS];
	const int minimum = sched_get_priority_min(SCHED_OTHER);
	const int maximum = sched_get_priority_max(SCHED_OTHER);
	bool passed = minimum == XSDK_PRIORITY_MIN && maximum == XSDK_PRIORITY_MAX;
	for (int level = XSDK_PRIORITY_MIN; level <= XSDK_PRIORITY_MAX; level++) {
		const struct sched_param desired = { .sched_priority = level };
		struct sched_param actual = { .sched_priority = INT_MIN };
		const unsigned index = level - XSDK_PRIORITY_MIN;
		policy = -1;
		samples[index].set = pthread_setschedparam(self, SCHED_OTHER, &desired);
		samples[index].get = pthread_getschedparam(self, &policy, &actual);
		samples[index].actual = actual.sched_priority;
		samples[index].policy = policy;
		passed &= samples[index].set == 0 && samples[index].get == 0 && actual.sched_priority == level &&
			policy == SCHED_OTHER;
	}
	const int restore_set = pthread_setschedparam(self, SCHED_OTHER, &original);
	struct sched_param restored_value = { .sched_priority = INT_MIN };
	const int restore_get = pthread_getschedparam(self, &policy, &restored_value);
	const bool restore_ok = !restore_set && !restore_get && restored_value.sched_priority == original.sched_priority;
	passed &= restore_ok;
	// Log after restoring the priority so logging does not affect the sweep.
	xsdk_log_write("thread control: range=%d..%d restore set=%d get=%d requested=%d actual=%d", minimum, maximum,
		restore_set, restore_get, original.sched_priority, restored_value.sched_priority);
	for (unsigned i = 0; i < LEVELS; i++)
		xsdk_log_write("thread control: priority requested=%d set=%d get=%d actual=%d policy=%d",
			(int)i + XSDK_PRIORITY_MIN, samples[i].set, samples[i].get, samples[i].actual, samples[i].policy);
	check("thread control: all priorities -16 through 16 read back and restore", passed);
	if (!restore_ok)
		exit(1);
	const struct sched_param invalid = { .sched_priority = 17 };
	check("thread control: invalid priority and hardware thread are refused",
		pthread_setschedparam(self, SCHED_OTHER, &invalid) == EINVAL &&
			xsdk_thread_place(xsdk_pthread_handle(self), XSDK_HARDWARE_THREADS) == STATUS_INVALID_PARAMETER);
	int32_t previous;
	const NTSTATUS changed = xsdk_thread_set_priority(XSDK_CURRENT_THREAD, 1, &previous);
	int32_t raised;
	const NTSTATUS restored = xsdk_thread_set_priority(XSDK_CURRENT_THREAD, original.sched_priority, &raised);
	check("thread control: pseudo handle sets priority and reports the previous value",
		NT_SUCCESS(changed) && previous == original.sched_priority && NT_SUCCESS(restored) && raised == 1);
}

typedef struct {
	HANDLE gate;
	int64_t end_ns;
	uint64_t passes;
} ShareWorker;

static uint32_t count_share(void *context)
{
	ShareWorker *worker = context;
	const int64_t timeout = -10000000;
	if (NtWaitForSingleObjectEx(worker->gate, USER_MODE, 0, &timeout) != STATUS_SUCCESS)
		return 1;
	while (now_ns() < worker->end_ns)
		worker->passes++;
	return 0;
}

static void priority_share(void)
{
	HANDLE gate;
	if (!NT_SUCCESS(NtCreateEvent(&gate, NULL, NOTIFICATION_EVENT, 0)))
		exit(1);
	ShareWorker workers[2] = { { .gate = gate }, { .gate = gate } };
	HANDLE threads[2];
	for (uint32_t i = 0; i < 2; i++)
		if (!NT_SUCCESS(xsdk_thread_create_on(&threads[i], WORKER_STACK, count_share, &workers[i], 1)))
			exit(1);
	const NTSTATUS normal = xsdk_thread_set_priority(threads[0], 0, NULL);
	const NTSTATUS raised = xsdk_thread_set_priority(threads[1], 2, NULL);
	workers[0].end_ns = workers[1].end_ns = now_ns() + 300000000;
	if (!NT_SUCCESS(NtSetEvent(gate, NULL)))
		exit(1);
	for (uint32_t i = 0; i < 2; i++)
		finish(threads[i]);
	xsdk_log_write("thread control: same-HT passes normal=%llu raised=%llu", (unsigned long long)workers[0].passes,
		(unsigned long long)workers[1].passes);
	check("thread control: a raised busy thread gets at least 95 percent on one hardware thread",
		NT_SUCCESS(normal) && NT_SUCCESS(raised) && workers[1].passes > 0 &&
			workers[1].passes >= workers[0].passes * 19);
	(void)NtClose(gate);
}

typedef struct {
	HANDLE gate;
	HANDLE ready;
	uint32_t observed;
	uint32_t stop;
} MoveWorker;

static uint32_t observe_place(void *context)
{
	MoveWorker *worker = context;
	if (worker->gate) {
		const int64_t timeout = -10000000;
		if (NtSignalAndWaitForSingleObjectEx(worker->ready, worker->gate, 0, 0, &timeout) != STATUS_SUCCESS)
			return 1;
		__atomic_store_n(&worker->observed, xsdk_thread_hardware_thread(), __ATOMIC_RELEASE);
		return 0;
	}
	const int64_t end = now_ns() + 1000000000;
	while (!__atomic_load_n(&worker->stop, __ATOMIC_ACQUIRE) && now_ns() < end)
		__atomic_store_n(&worker->observed, xsdk_thread_hardware_thread(), __ATOMIC_RELEASE);
	return 0;
}

static void place_threads(void)
{
	MoveWorker busy = { .observed = UINT32_MAX };
	HANDLE thread;
	if (!NT_SUCCESS(xsdk_thread_create_on(&thread, WORKER_STACK, observe_place, &busy, 2)))
		exit(1);
	int64_t end = now_ns() + 100000000;
	while (__atomic_load_n(&busy.observed, __ATOMIC_ACQUIRE) != 2 && now_ns() < end)
		(void)xsdk_thread_sleep(1);
	const bool before = __atomic_load_n(&busy.observed, __ATOMIC_ACQUIRE) == 2;
	const NTSTATUS moved = xsdk_thread_place(thread, 3);
	end = now_ns() + 100000000;
	while (__atomic_load_n(&busy.observed, __ATOMIC_ACQUIRE) != 3 && now_ns() < end)
		(void)xsdk_thread_sleep(1);
	__atomic_store_n(&busy.stop, 1, __ATOMIC_RELEASE);
	finish(thread);
	check("thread control: another busy thread moves from hardware thread 2 to 3",
		before && NT_SUCCESS(moved) && busy.observed == 3);
	MoveWorker asleep = { .observed = UINT32_MAX };
	if (!NT_SUCCESS(NtCreateEvent(&asleep.gate, NULL, NOTIFICATION_EVENT, 0)) ||
		!NT_SUCCESS(NtCreateEvent(&asleep.ready, NULL, NOTIFICATION_EVENT, 0)) ||
		!NT_SUCCESS(xsdk_thread_create_on(&thread, WORKER_STACK, observe_place, &asleep, 2)))
		exit(1);
	const int64_t timeout = -10000000;
	if (NtWaitForSingleObjectEx(asleep.ready, USER_MODE, 0, &timeout) != STATUS_SUCCESS)
		exit(1);
	const NTSTATUS moved_asleep = xsdk_thread_place(thread, 4);
	if (!NT_SUCCESS(NtSetEvent(asleep.gate, NULL)))
		exit(1);
	finish(thread);
	check("thread control: a waiting thread wakes on its new hardware thread",
		NT_SUCCESS(moved_asleep) && asleep.observed == 4);
	(void)NtClose(asleep.ready);
	(void)NtClose(asleep.gate);
}

void test_thread_control(void)
{
	const uint32_t original = xsdk_thread_hardware_thread();
	if (!NT_SUCCESS(xsdk_thread_move(0)))
		exit(1);
	priorities();
	priority_share();
	place_threads();
	check("thread control: the pseudo handle places the caller",
		NT_SUCCESS(xsdk_thread_place(XSDK_CURRENT_THREAD, original)) && xsdk_thread_hardware_thread() == original);
}
