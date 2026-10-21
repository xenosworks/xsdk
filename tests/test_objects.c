#include "tests.h"

#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

enum {
	WORKER_STACK = 64 * 1024,
	ENTRIES = 1000,
	TIMER_PERIOD_MS = 5,
	TIMER_FIRES = 20,
	TIMER_WAIT_MS = 50,
	// The console adds roughly one millisecond per expiration (docs/HARDWARE.md, M55 acceptance failures).
	TIMER_TICK_NS = 2000000,
};

static NTSTATUS wait_for(HANDLE handle, uint32_t ms)
{
	const int64_t timeout = -(int64_t)ms * 10000;
	return NtWaitForSingleObjectEx(handle, USER_MODE, 0, &timeout);
}

static void finish(HANDLE thread)
{
	if (wait_for(thread, 2000) != STATUS_SUCCESS) {
		check("objects: worker finishes before its context is released", false);
		exit(1);
	}
	(void)NtClose(thread);
}

static bool event(HANDLE *handle, uint32_t state)
{
	const NTSTATUS status = NtCreateEvent(handle, NULL, NOTIFICATION_EVENT, state);
	if (!NT_SUCCESS(status))
		check("objects: create event", false);
	return NT_SUCCESS(status);
}

static void waits(void)
{
	HANDLE handles[MAXIMUM_WAIT_OBJECTS + 1];
	for (uint32_t i = 0; i < MAXIMUM_WAIT_OBJECTS + 1; i++)
		if (!event(&handles[i], i >= 2))
			return;
	const int64_t poll = 0;
	check("objects: wait-any returns the signalled index",
		NtWaitForMultipleObjectsEx(3, handles, WAIT_ANY, USER_MODE, 0, &poll) == STATUS_WAIT_0 + 2);
	const bool set0 = NT_SUCCESS(NtSetEvent(handles[0], NULL));
	const NTSTATUS missing = NtWaitForMultipleObjectsEx(3, handles, WAIT_ALL, USER_MODE, 0, &poll);
	const bool set1 = NT_SUCCESS(NtSetEvent(handles[1], NULL));
	check("objects: wait-all needs all three objects",
		set0 && set1 && missing == STATUS_TIMEOUT &&
			NtWaitForMultipleObjectsEx(3, handles, WAIT_ALL, USER_MODE, 0, &poll) == STATUS_SUCCESS);
	check("objects: 64 waits succeed and 65 are refused",
		NtWaitForMultipleObjectsEx(MAXIMUM_WAIT_OBJECTS, handles, WAIT_ALL, USER_MODE, 0, &poll) == STATUS_SUCCESS &&
			NtWaitForMultipleObjectsEx(MAXIMUM_WAIT_OBJECTS + 1, handles, WAIT_ALL, USER_MODE, 0, &poll) ==
				STATUS_INVALID_PARAMETER);
	const NTSTATUS cleared = NtClearEvent(handles[0]);
	check(
		"objects: clear leaves an event unsignalled", NT_SUCCESS(cleared) && wait_for(handles[0], 0) == STATUS_TIMEOUT);
	for (uint32_t i = 0; i < MAXIMUM_WAIT_OBJECTS + 1; i++)
		(void)NtClose(handles[i]);
}

typedef struct {
	HANDLE ready;
	HANDLE wake;
	NTSTATUS result;
} EventWorker;

static uint32_t signal_wait(void *context)
{
	EventWorker *worker = context;
	const int64_t timeout = -10000000;
	worker->result = NtSignalAndWaitForSingleObjectEx(worker->ready, worker->wake, 0, 0, &timeout);
	return 0;
}

static void pulses(void)
{
	HANDLE pulse;
	if (!event(&pulse, 0))
		return;
	EventWorker workers[2] = { { .wake = pulse }, { .wake = pulse } };
	HANDLE threads[2];
	for (uint32_t i = 0; i < 2; i++) {
		if (!event(&workers[i].ready, 0) ||
			!NT_SUCCESS(xsdk_thread_create(&threads[i], WORKER_STACK, signal_wait, &workers[i])))
			exit(1);
		if (wait_for(workers[i].ready, 1000) != STATUS_SUCCESS)
			exit(1);
	}
	int32_t previous = -1;
	(void)xsdk_thread_sleep(20);
	const bool waiting = wait_for(threads[0], 0) == STATUS_TIMEOUT && wait_for(threads[1], 0) == STATUS_TIMEOUT;
	const NTSTATUS pulsed = NtPulseEvent(pulse, &previous);
	for (uint32_t i = 0; i < 2; i++)
		finish(threads[i]);
	check("objects: signal-and-wait signals ready and wakes on the pulse",
		waiting && workers[0].result == STATUS_SUCCESS && workers[1].result == STATUS_SUCCESS);
	check("objects: manual pulse wakes both waiters and leaves clear",
		NT_SUCCESS(pulsed) && previous == 0 && workers[0].result == STATUS_SUCCESS &&
			workers[1].result == STATUS_SUCCESS && wait_for(pulse, 0) == STATUS_TIMEOUT);
	for (uint32_t i = 0; i < 2; i++)
		(void)NtClose(workers[i].ready);
	(void)NtClose(pulse);
}

typedef struct {
	HANDLE mutant;
	HANDLE attempted;
	NTSTATUS not_owner;
	NTSTATUS acquired;
	NTSTATUS released;
} MutantWorker;

static uint32_t take_mutant(void *context)
{
	MutantWorker *worker = context;
	worker->not_owner = NtReleaseMutant(worker->mutant, 0);
	if (!NT_SUCCESS(NtSetEvent(worker->attempted, NULL)))
		return 1;
	worker->acquired = wait_for(worker->mutant, 1000);
	if (worker->acquired == STATUS_SUCCESS)
		worker->released = NtReleaseMutant(worker->mutant, 0);
	return 0;
}

static uint32_t abandon_mutant(void *context)
{
	MutantWorker *worker = context;
	worker->acquired = wait_for(worker->mutant, 1000);
	return 0;
}

static void mutants(void)
{
	MutantWorker worker = { .not_owner = STATUS_SUCCESS, .acquired = STATUS_TIMEOUT, .released = STATUS_TIMEOUT };
	if (!NT_SUCCESS(NtCreateMutant(&worker.mutant, NULL, 1)) || !event(&worker.attempted, 0)) {
		check("objects: create mutant", false);
		return;
	}
	const NTSTATUS acquired = wait_for(worker.mutant, 0);
	HANDLE thread;
	if (!NT_SUCCESS(xsdk_thread_create(&thread, WORKER_STACK, take_mutant, &worker)))
		return;
	if (wait_for(worker.attempted, 1000) != STATUS_SUCCESS)
		exit(1);
	const NTSTATUS first = NtReleaseMutant(worker.mutant, 0);
	const NTSTATUS blocked = wait_for(thread, 20);
	const NTSTATUS second = NtReleaseMutant(worker.mutant, 0);
	finish(thread);
	check("objects: a non-owner cannot release a mutant", worker.not_owner == STATUS_MUTANT_NOT_OWNED);
	check("objects: recursive ownership blocks another thread until both releases",
		acquired == STATUS_SUCCESS && NT_SUCCESS(first) && blocked == STATUS_TIMEOUT && NT_SUCCESS(second) &&
			worker.acquired == STATUS_SUCCESS && NT_SUCCESS(worker.released));
	worker.acquired = STATUS_TIMEOUT;
	if (!NT_SUCCESS(xsdk_thread_create(&thread, WORKER_STACK, abandon_mutant, &worker)))
		return;
	finish(thread);
	const NTSTATUS abandoned = wait_for(worker.mutant, 1000);
	check("objects: waiting for an abandoned mutant reports abandonment",
		worker.acquired == STATUS_SUCCESS && abandoned == STATUS_ABANDONED_WAIT_0);
	if (abandoned == STATUS_ABANDONED_WAIT_0 || abandoned == STATUS_SUCCESS)
		(void)NtReleaseMutant(worker.mutant, 0);
	(void)NtClose(worker.attempted);
	(void)NtClose(worker.mutant);
}

static int64_t now_ns(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static void timers(void)
{
	HANDLE timer;
	if (!NT_SUCCESS(NtCreateTimer(&timer, NULL, SYNCHRONIZATION_TIMER))) {
		check("objects: create timer", false);
		return;
	}
	const int64_t due = -50000;
	const int64_t start = now_ns();
	const NTSTATUS set = NtSetTimerEx(timer, &due, NULL, 1, NULL, 0, 0, 0);
	const NTSTATUS fired = wait_for(timer, 1000);
	const int64_t elapsed = now_ns() - start;
	xsdk_log_write("objects: 5 ms one-shot set=0x%08X wait=0x%08X elapsed=%lld ns", (unsigned)set, (unsigned)fired,
		(long long)elapsed);
	check("objects: a 5 ms one-shot fires once within a 2 ms tick",
		NT_SUCCESS(set) && fired == STATUS_SUCCESS && elapsed >= 3000000 && elapsed <= 7000000 &&
			wait_for(timer, 0) == STATUS_TIMEOUT);
	const int64_t period_start = now_ns();
	const NTSTATUS periodic = NtSetTimerEx(timer, &due, NULL, 1, NULL, 0, TIMER_PERIOD_MS, 0);
	uint32_t fires = 0;
	NTSTATUS last_wait = periodic;
	int64_t previous = period_start;
	int64_t minimum = INT64_MAX;
	int64_t maximum = 0;
	while (NT_SUCCESS(periodic) && fires < TIMER_FIRES) {
		last_wait = wait_for(timer, TIMER_WAIT_MS);
		if (last_wait != STATUS_SUCCESS)
			break;
		const int64_t now = now_ns();
		const int64_t interval = now - previous;
		if (interval < minimum)
			minimum = interval;
		if (interval > maximum)
			maximum = interval;
		previous = now;
		fires++;
	}
	const int64_t period_elapsed = now_ns() - period_start;
	uint32_t state = 0;
	const NTSTATUS cancelled = NtCancelTimer(timer, &state);
	// Consume a signal that raced with cancellation before checking that no new one arrives.
	const NTSTATUS drained = wait_for(timer, 0);
	const NTSTATUS stopped = wait_for(timer, TIMER_WAIT_MS);
	xsdk_log_write("objects: periodic set=0x%08X last_wait=0x%08X fires=%u elapsed=%lld ns intervals=%lld..%lld ns",
		(unsigned)periodic, (unsigned)last_wait, (unsigned)fires, (long long)period_elapsed,
		(long long)(fires ? minimum : 0), (long long)maximum);
	const int64_t period_ns = TIMER_PERIOD_MS * 1000000;
	check("objects: 20 periodic firings allow 2 ms lateness per period",
		NT_SUCCESS(periodic) && last_wait == STATUS_SUCCESS && fires == TIMER_FIRES &&
			period_elapsed >= TIMER_FIRES * (period_ns - TIMER_TICK_NS) &&
			period_elapsed <= TIMER_FIRES * (period_ns + TIMER_TICK_NS));
	xsdk_log_write("objects: cancel=0x%08X state=%u drain=0x%08X after=0x%08X", (unsigned)cancelled, (unsigned)state,
		(unsigned)drained, (unsigned)stopped);
	check("objects: a cancelled timer stops firing",
		NT_SUCCESS(cancelled) && (drained == STATUS_SUCCESS || drained == STATUS_TIMEOUT) && stopped == STATUS_TIMEOUT);
	(void)NtClose(timer);
}

typedef struct {
	SINGLE_LIST_ENTRY link;
	uint32_t index;
} Entry;

typedef struct {
	SLIST_HEADER *list;
	Entry *entries;
	uint32_t first;
} ListWorker;

static uint32_t push_entries(void *context)
{
	ListWorker *worker = context;
	for (uint32_t i = 0; i < ENTRIES; i++) {
		Entry *entry = &worker->entries[i];
		entry->index = worker->first + i;
		(void)InterlockedPushEntrySList(worker->list, &entry->link);
	}
	return 0;
}

static void lists(void)
{
	SLIST_HEADER list = { 0 };
	Entry entries[2 * ENTRIES];
	ListWorker workers[2] = { { &list, entries, 0 }, { &list, entries + ENTRIES, ENTRIES } };
	HANDLE threads[2];
	for (uint32_t i = 0; i < 2; i++)
		if (!NT_SUCCESS(xsdk_thread_create_on(&threads[i], WORKER_STACK, push_entries, &workers[i], 2 * i)))
			exit(1);
	for (uint32_t i = 0; i < 2; i++)
		finish(threads[i]);
	bool seen[2 * ENTRIES] = { false };
	bool unique = true;
	uint32_t count = 0;
	uint32_t address;
	while ((address = InterlockedPopEntrySList(&list)) != 0 && count <= 2 * ENTRIES) {
		const Entry *entry = (const Entry *)(uintptr_t)address;
		if (address < (uintptr_t)entries || address >= (uintptr_t)(entries + 2 * ENTRIES) ||
			entry->index >= 2 * ENTRIES) {
			unique = false;
			break;
		}
		unique &= !seen[entry->index];
		seen[entry->index] = true;
		count++;
	}
	check("objects: concurrent list pushes pop 2000 unique entries", unique && count == 2 * ENTRIES && !address);
	(void)InterlockedPushEntrySList(&list, &entries[0].link);
	(void)InterlockedPushEntrySList(&list, &entries[1].link);
	const uint32_t flushed = InterlockedFlushSList(&list);
	check("objects: list flush returns the chain and empties the list",
		flushed == (uint32_t)(uintptr_t)&entries[1] && entries[1].link.next == (uint32_t)(uintptr_t)&entries[0] &&
			InterlockedPopEntrySList(&list) == 0);
}

static uint32_t return_zero(void *context)
{
	(void)context;
	return 0;
}

static void mixed_wait(void)
{
	HANDLE handles[4] = { 0 };
	const NTSTATUS created[] = {
		NtCreateEvent(&handles[0], NULL, NOTIFICATION_EVENT, 1),
		NtCreateSemaphore(&handles[1], NULL, 1, 1),
		NtCreateMutant(&handles[2], NULL, 0),
		xsdk_thread_create(&handles[3], WORKER_STACK, return_zero, NULL),
	};
	const char *const names[] = { "event", "semaphore", "mutant", "thread" };
	for (unsigned i = 0; i < 4; i++) {
		xsdk_log_write(
			"objects: mixed %s create=0x%08X handle=%08X", names[i], (unsigned)created[i], (unsigned)handles[i]);
		if (!NT_SUCCESS(created[i])) {
			check("objects: mixed wait setup", false);
			exit(1);
		}
	}
	const NTSTATUS ready = wait_for(handles[3], 1000);
	xsdk_log_write("objects: mixed completed thread status=0x%08X", (unsigned)ready);
	check("objects: mixed ready control starts with a completed thread", ready == STATUS_SUCCESS);
	const int64_t timeout = -10000000;
	const int64_t start = now_ns();
	const NTSTATUS status = NtWaitForMultipleObjectsEx(4, handles, WAIT_ALL, USER_MODE, 0, &timeout);
	xsdk_log_write(
		"objects: mixed wait-all status=0x%08X elapsed=%lld ns", (unsigned)status, (long long)(now_ns() - start));
	check("objects: wait-all accepts an event, semaphore, mutant and thread", status == STATUS_SUCCESS);
	if (status == STATUS_SUCCESS || (status >= STATUS_ABANDONED_WAIT_0 && status < STATUS_ABANDONED_WAIT_0 + 4)) {
		const NTSTATUS released = NtReleaseMutant(handles[2], 0);
		xsdk_log_write("objects: mixed mutant release=0x%08X", (unsigned)released);
		check("objects: mixed wait releases acquired mutant", NT_SUCCESS(released));
	}
	if (status != STATUS_SUCCESS) {
		for (unsigned i = 0; i < 4; i++) {
			const NTSTATUS single = wait_for(handles[i], 0);
			xsdk_log_write("objects: mixed diagnostic single %s=0x%08X", names[i], (unsigned)single);
			if (i == 2 && (single == STATUS_SUCCESS || single == STATUS_ABANDONED_WAIT_0)) {
				const NTSTATUS released = NtReleaseMutant(handles[i], 0);
				xsdk_log_write("objects: mixed diagnostic mutant release=0x%08X", (unsigned)released);
				check("objects: diagnostic releases acquired mutant", NT_SUCCESS(released));
			}
		}
	}
	finish(handles[3]);
	for (uint32_t i = 0; i < 3; i++) {
		const NTSTATUS closed = NtClose(handles[i]);
		if (!NT_SUCCESS(closed))
			xsdk_log_write("objects: mixed close %s=0x%08X", names[i], (unsigned)closed);
		check("objects: mixed handle closes", NT_SUCCESS(closed));
	}
}

typedef struct {
	HANDLE handles[4];
	HANDLE ready;
	NTSTATUS signalled;
	NTSTATUS waited;
	NTSTATUS released;
	int64_t elapsed_ns;
} MixedWaitWorker;

static uint32_t wait_for_mixed(void *context)
{
	MixedWaitWorker *worker = context;
	worker->signalled = NtSetEvent(worker->ready, NULL);
	if (!NT_SUCCESS(worker->signalled))
		return 1;
	const int64_t timeout = -10000000;
	const int64_t start = now_ns();
	worker->waited = NtWaitForMultipleObjectsEx(4, worker->handles, WAIT_ALL, USER_MODE, 0, &timeout);
	worker->elapsed_ns = now_ns() - start;
	// The waiting thread must release its own mutant, even when diagnosing an unexpected wait result.
	worker->released = NtReleaseMutant(worker->handles[2], 0);
	return 0;
}

static void mixed_pending(void)
{
	EventWorker target = { .result = STATUS_TIMEOUT };
	MixedWaitWorker worker = { .signalled = STATUS_TIMEOUT, .waited = STATUS_TIMEOUT, .released = STATUS_TIMEOUT };
	if (!event(&target.ready, 0) || !event(&target.wake, 0) || !event(&worker.ready, 0))
		exit(1);
	const NTSTATUS created[] = {
		NtCreateEvent(&worker.handles[0], NULL, NOTIFICATION_EVENT, 1),
		NtCreateSemaphore(&worker.handles[1], NULL, 1, 1),
		NtCreateMutant(&worker.handles[2], NULL, 0),
		xsdk_thread_create_on(&worker.handles[3], WORKER_STACK, signal_wait, &target, 2),
	};
	for (unsigned i = 0; i < 4; i++) {
		if (!NT_SUCCESS(created[i])) {
			xsdk_log_write("objects: pending create index=%u status=0x%08X", i, (unsigned)created[i]);
			check("objects: pending mixed wait setup", false);
			exit(1);
		}
	}
	const NTSTATUS target_ready = wait_for(target.ready, 1000);
	HANDLE waiter;
	const NTSTATUS waiter_created = xsdk_thread_create_on(&waiter, WORKER_STACK, wait_for_mixed, &worker, 4);
	if (!NT_SUCCESS(waiter_created)) {
		xsdk_log_write("objects: pending waiter create=0x%08X", (unsigned)waiter_created);
		check("objects: pending waiter setup", false);
		exit(1);
	}
	const NTSTATUS waiter_ready = wait_for(worker.ready, 1000);
	// No logging in this window: the target stays behind its gate while the mixed waiter has time to block.
	const NTSTATUS held = wait_for(waiter, 20);
	const NTSTATUS target_held = wait_for(worker.handles[3], 0);
	const NTSTATUS opened = NtSetEvent(target.wake, NULL);
	finish(waiter);
	finish(worker.handles[3]);
	const NTSTATUS token = wait_for(worker.handles[1], 0);
	xsdk_log_write("objects: pending ready target=0x%08X waiter=0x%08X signal=0x%08X held=0x%08X target=0x%08X",
		(unsigned)target_ready, (unsigned)waiter_ready, (unsigned)worker.signalled, (unsigned)held,
		(unsigned)target_held);
	xsdk_log_write(
		"objects: pending gate=0x%08X target wait=0x%08X mixed=0x%08X elapsed=%lld ns release=0x%08X token=0x%08X",
		(unsigned)opened, (unsigned)target.result, (unsigned)worker.waited, (long long)worker.elapsed_ns,
		(unsigned)worker.released, (unsigned)token);
	check("objects: mixed wait-all stays pending while the target thread is gated",
		target_ready == STATUS_SUCCESS && waiter_ready == STATUS_SUCCESS && NT_SUCCESS(worker.signalled) &&
			held == STATUS_TIMEOUT && target_held == STATUS_TIMEOUT);
	check("objects: pending mixed wait-all completes after the target thread ends",
		NT_SUCCESS(opened) && target.result == STATUS_SUCCESS && worker.waited == STATUS_WAIT_0 + 3);
	check("objects: pending mixed wait takes the semaphore and owns the mutant",
		token == STATUS_TIMEOUT && NT_SUCCESS(worker.released));
	for (unsigned i = 0; i < 3; i++)
		check("objects: pending mixed handle closes", NT_SUCCESS(NtClose(worker.handles[i])));
	check("objects: pending gate handles close",
		NT_SUCCESS(NtClose(worker.ready)) && NT_SUCCESS(NtClose(target.wake)) && NT_SUCCESS(NtClose(target.ready)));
}

void test_objects(void)
{
	xsdk_log_write("objects: M55 acceptance diagnostics v4");
	waits();
	pulses();
	mutants();
	timers();
	lists();
	mixed_wait();
	mixed_pending();
}
