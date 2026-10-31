#include "tests.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "xsdk/log.h"
#include "xsdk/thread.h"

enum {
	// Away from the kernel's default, so a thread given it can't pass with the default stack.
	ASKED_STACK_BYTES = 384 * 1024,
	// The kernel may round a stack up to its own unit.
	STACK_SLACK_BYTES = 64 * 1024,
	// Where the processor control region keeps the stack's high and low ends (xenia:src/xenia/kernel/xthread.h:78-79).
	PCR_STACK_BASE = 0x70,
	PCR_STACK_END = 0x74,
	DETACHED_WAIT_MS = 5000,
	// Long for a thread that ends at once.
	EXIT_WAIT_MS = 1000,
	NANOSECONDS_PER_MS = 1000000,
	CONDITION_WAIT_MS = 100,
	// The kernel's wait can end up to 1.4 ms early (docs/HARDWARE.md, "The time base runs at 49.875 MHz"), and the
	// system time steps a millisecond at a time.
	CONDITION_EARLY_NS = 3 * NANOSECONDS_PER_MS,
	CONDITION_LATE_NS = 50 * NANOSECONDS_PER_MS,
	// No clock has it.
	UNKNOWN_CLOCK = 99,
};

static uint32_t detached_ran;
static pthread_key_t exit_key;
static uint32_t exit_destructor_runs;
static uint32_t ran_past_exit;
static int exit_value;

static uint32_t current_stack_bytes(void)
{
	const uint8_t *pcr;
	__asm__("mr %0, 13" : "=r"(pcr));
	const uint32_t base = *(const volatile uint32_t *)(pcr + PCR_STACK_BASE);
	const uint32_t end = *(const volatile uint32_t *)(pcr + PCR_STACK_END);
	return base - end;
}

static void *report_stack(void *argument)
{
	(void)argument;
	return (void *)(uintptr_t)current_stack_bytes();
}

static void *mark_detached(void *argument)
{
	(void)argument;
	const HANDLE handle = xsdk_pthread_handle(pthread_self());
	uint32_t object;
	const NTSTATUS status = ObReferenceObjectByHandle(handle, (uint32_t)(uintptr_t)ExThreadObjectType, &object);
	if (NT_SUCCESS(status))
		(void)ObDereferenceObject(object);
	__atomic_store_n(&detached_ran, NT_SUCCESS(status) ? 1 : 2, __ATOMIC_RELEASE);
	return NULL;
}

// The stack a thread started with these attributes has, or 0 if it didn't start.
static uint32_t started_stack_bytes(const pthread_attr_t *attr)
{
	pthread_t thread;
	if (pthread_create(&thread, attr, report_stack, NULL) != 0)
		return 0;
	void *result = NULL;
	if (pthread_join(thread, &result) != 0)
		return 0;
	return (uint32_t)(uintptr_t)result;
}

static bool detached_thread_runs(void)
{
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_t thread;
	const int created = pthread_create(&thread, &attr, mark_detached, NULL);
	pthread_attr_destroy(&attr);
	if (created != 0)
		return false;
	const struct timespec millisecond = { .tv_nsec = 1000000 };
	for (int waited_ms = 0; waited_ms < DETACHED_WAIT_MS; waited_ms++) {
		if (__atomic_load_n(&detached_ran, __ATOMIC_ACQUIRE))
			return __atomic_load_n(&detached_ran, __ATOMIC_RELAXED) == 1;
		(void)nanosleep(&millisecond, NULL);
	}
	return false;
}

static void count_exit_destructor(void *value)
{
	if (value == &exit_value)
		__atomic_fetch_add(&exit_destructor_runs, 1, __ATOMIC_RELAXED);
}

// A call down, as pthread_exit ends the thread from anywhere in it.
static void set_key_and_exit(void *result)
{
	(void)pthread_setspecific(exit_key, &exit_value);
	pthread_exit(result);
}

static void *exit_early(void *argument)
{
	set_key_and_exit(argument);
	__atomic_store_n(&ran_past_exit, 1, __ATOMIC_RELAXED);
	return NULL;
}

static uint32_t exit_early_entry(void *argument)
{
	set_key_and_exit(argument);
	__atomic_store_n(&ran_past_exit, 1, __ATOMIC_RELAXED);
	return 0;
}

static bool exit_reaches_join(void)
{
	__atomic_store_n(&exit_destructor_runs, 0, __ATOMIC_RELAXED);
	pthread_t thread;
	void *result = NULL;
	const bool joined =
		pthread_create(&thread, NULL, exit_early, &exit_value) == 0 && pthread_join(thread, &result) == 0;
	return joined && result == &exit_value && __atomic_load_n(&exit_destructor_runs, __ATOMIC_RELAXED) == 1 &&
		!__atomic_load_n(&ran_past_exit, __ATOMIC_RELAXED);
}

// Waits for the destructor to have run count times in all.
static bool destructor_ran(uint32_t count)
{
	const struct timespec millisecond = { .tv_nsec = NANOSECONDS_PER_MS };
	for (int waited_ms = 0; waited_ms < EXIT_WAIT_MS; waited_ms++) {
		if (__atomic_load_n(&exit_destructor_runs, __ATOMIC_RELAXED) == count)
			return true;
		(void)nanosleep(&millisecond, NULL);
	}
	return false;
}

static bool exit_ends_other_threads(void)
{
	__atomic_store_n(&exit_destructor_runs, 0, __ATOMIC_RELAXED);
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_t detached;
	const bool detached_ended = pthread_create(&detached, &attr, exit_early, NULL) == 0 && destructor_ran(1);
	pthread_attr_destroy(&attr);

	HANDLE handle;
	if (!NT_SUCCESS(xsdk_thread_create(&handle, 0, exit_early_entry, NULL)))
		return false;
	const int64_t timeout = -(int64_t)EXIT_WAIT_MS * 10000;
	const bool kernel_ended = NtWaitForSingleObjectEx(handle, USER_MODE, 0, &timeout) == STATUS_SUCCESS;
	(void)NtClose(handle);
	return detached_ended && kernel_ended && __atomic_load_n(&exit_destructor_runs, __ATOMIC_RELAXED) == 2 &&
		!__atomic_load_n(&ran_past_exit, __ATOMIC_RELAXED);
}

static bool attributes_read_back(void)
{
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	int state = -1;
	size_t stack_bytes = 1;
	const bool defaults = pthread_attr_getdetachstate(&attr, &state) == 0 && state == PTHREAD_CREATE_JOINABLE &&
		pthread_attr_getstacksize(&attr, &stack_bytes) == 0 && stack_bytes == 0;
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&attr, ASKED_STACK_BYTES);
	const bool set = pthread_attr_getdetachstate(&attr, &state) == 0 && state == PTHREAD_CREATE_DETACHED &&
		pthread_attr_getstacksize(&attr, &stack_bytes) == 0 && stack_bytes == ASKED_STACK_BYTES;
	pthread_attr_destroy(&attr);
	return defaults && set;
}

static int64_t monotonic_ns(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

// How long, on CLOCK_MONOTONIC, a timed wait on a condition made with attr lasts, CONDITION_WAIT_MS ahead on clock;
// -1 if it doesn't time out.
static int64_t timed_wait_ns(const pthread_condattr_t *attr, clockid_t clock)
{
	pthread_cond_t cond;
	pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	if (pthread_cond_init(&cond, attr) != 0)
		return -1;
	struct timespec deadline;
	(void)clock_gettime(clock, &deadline);
	deadline.tv_nsec += CONDITION_WAIT_MS * NANOSECONDS_PER_MS;
	deadline.tv_sec += deadline.tv_nsec / 1000000000;
	deadline.tv_nsec %= 1000000000;
	const int64_t start = monotonic_ns();
	(void)pthread_mutex_lock(&mutex);
	const int result = pthread_cond_timedwait(&cond, &mutex, &deadline);
	(void)pthread_mutex_unlock(&mutex);
	const int64_t waited = monotonic_ns() - start;
	(void)pthread_cond_destroy(&cond);
	(void)pthread_mutex_destroy(&mutex);
	return result == ETIMEDOUT ? waited : -1;
}

static bool waited_until_deadline(int64_t waited_ns)
{
	return waited_ns >= CONDITION_WAIT_MS * NANOSECONDS_PER_MS - CONDITION_EARLY_NS &&
		waited_ns < CONDITION_WAIT_MS * NANOSECONDS_PER_MS + CONDITION_LATE_NS;
}

// A condition on CLOCK_MONOTONIC whose wait read its deadline on CLOCK_REALTIME would find it decades past.
static void check_condition_clocks(void)
{
	pthread_condattr_t attr;
	pthread_condattr_init(&attr);
	clockid_t clock = CLOCK_REALTIME;
	const bool set = pthread_condattr_setclock(&attr, UNKNOWN_CLOCK) == EINVAL &&
		pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0 && pthread_condattr_getclock(&attr, &clock) == 0 &&
		clock == CLOCK_MONOTONIC;
	const int64_t realtime_wait_ns = timed_wait_ns(NULL, CLOCK_REALTIME);
	const int64_t monotonic_wait_ns = timed_wait_ns(&attr, CLOCK_MONOTONIC);
	pthread_condattr_destroy(&attr);
	xsdk_log_write("pthread: timed waits of %d ms took %lld ns on CLOCK_REALTIME, %lld ns on CLOCK_MONOTONIC",
		CONDITION_WAIT_MS, (long long)realtime_wait_ns, (long long)monotonic_wait_ns);
	check(
		"pthread: a condition's timed wait lasts until its deadline on CLOCK_REALTIME, or on CLOCK_MONOTONIC set in "
		"its attributes",
		set && waited_until_deadline(realtime_wait_ns) && waited_until_deadline(monotonic_wait_ns));
}

static bool bad_attributes_refused(void)
{
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	const bool refused =
		pthread_attr_setdetachstate(&attr, -1) == EINVAL && pthread_attr_setstacksize(&attr, 1) == EINVAL;
	pthread_attr_destroy(&attr);
	return refused;
}

void test_pthread(void)
{
	const uint32_t default_bytes = started_stack_bytes(NULL);
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, ASKED_STACK_BYTES);
	const uint32_t asked_bytes = started_stack_bytes(&attr);
	pthread_attr_destroy(&attr);
	xsdk_log_write("pthread: a thread's stack is %u bytes by default, %u when %u are asked for",
		(unsigned)default_bytes, (unsigned)asked_bytes, (unsigned)ASKED_STACK_BYTES);
	check("pthread: a thread gets the stack size its attributes ask for",
		asked_bytes >= ASKED_STACK_BYTES && asked_bytes < ASKED_STACK_BYTES + STACK_SLACK_BYTES);
	check("pthread: a thread made detached retains its kernel handle", detached_thread_runs());
	check("pthread: a bad detach state and a one-byte stack are refused", bad_attributes_refused());
	check("pthread: attributes read back their detach state and stack size, 0 for the .xex's default stack",
		attributes_read_back());

	if (pthread_key_create(&exit_key, count_exit_destructor) != 0) {
		check("pthread_exit: the key its test needs is made", false);
		return;
	}
	check("pthread_exit: its value reaches pthread_join, the thread's key destructor runs, and nothing after it does",
		exit_reaches_join());
	check("pthread_exit: a detached thread and one xsdk_thread_create made end there, each running its key destructor",
		exit_ends_other_threads());
	(void)pthread_key_delete(exit_key);
	check_condition_clocks();
}
