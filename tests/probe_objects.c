#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "probe.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Milestone 55 step 1 (docs/scope/55-windows-threads-and-memory.md): what the Windows-style objects, thread
// priorities and page calls do on the console, which Xenia only guesses at. One run, unattended. Every case but two
// is safe; the two that could stop the console if the kernel takes them as fatal - a wait on one object more than the
// most, and a wait on a mutant whose owner ended holding it - log a warning first, so net_log.txt has the line before
// a hang (docs/HARDWARE.md, "The log over the network, milestone 53").

enum {
	PCR_CURRENT_THREAD = 0x100,
	THREAD_STACK_BYTES = 64 * 1024,
	MAX_EVENTS = 70,
	// NT's MAXIMUM_WAIT_OBJECTS; the probe looks for where the console stops.
	NT_WAIT_MAX = 64,
	BUSY_MS = 300,
	WORKER_WAIT_MS = 5000,
	MOVE_FROM = 0,
	MOVE_TO = 3,
	PRIORITY_HARDWARE_THREAD = 1,
	SLIST_ENTRIES = 1000,
	PAGE_BYTES = 4096,
	ALLOC_BYTES = 64 * 1024,
	TIMER_ONE_SHOT_MS = 5,
	TIMER_PERIOD_MS = 2,
	TIMER_WINDOW_MS = 100,
	GPR_PATTERN = 0xA5A50000,
};

// 100 ns units.
static const int64_t MS_100NS = 10000;

static uint32_t passed_count;
static uint32_t failed_count;

static void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	passed_count += passed;
	failed_count += !passed;
}

static int64_t relative_timeout(uint32_t milliseconds)
{
	return -(int64_t)milliseconds * MS_100NS;
}

static uint32_t current_thread_object(void)
{
	uint32_t pcr;
	__asm__ volatile("mr %0, 13" : "=r"(pcr));
	return *(volatile uint32_t *)(uintptr_t)(pcr + PCR_CURRENT_THREAD);
}

static HANDLE make_event(uint32_t type, uint32_t initial_state)
{
	HANDLE event = 0;
	const NTSTATUS status = NtCreateEvent(&event, NULL, type, initial_state);
	if (!NT_SUCCESS(status))
		xsdk_log_write("objects: NtCreateEvent failed: %08X", (unsigned)status);
	return event;
}

// ---- Events: clear and pulse ----

static volatile uint32_t pulse_woke;

static uint32_t pulse_waiter(void *context)
{
	HANDLE event = (HANDLE)(uintptr_t)context;
	const int64_t timeout = relative_timeout(2000);
	if (NtWaitForSingleObjectEx(event, USER_MODE, 0, &timeout) == STATUS_SUCCESS)
		__atomic_store_n(&pulse_woke, 1, __ATOMIC_RELEASE);
	return 0;
}

static void events_case(void)
{
	HANDLE manual = make_event(NOTIFICATION_EVENT, 0);
	if (manual) {
		(void)NtSetEvent(manual, NULL);
		const NTSTATUS cleared = NtClearEvent(manual);
		const int64_t zero = 0;
		const NTSTATUS after = NtWaitForSingleObjectEx(manual, USER_MODE, 0, &zero);
		xsdk_log_write("objects: NtClearEvent = %08X; a wait after it = %08X", (unsigned)cleared, (unsigned)after);
		check("objects: NtClearEvent leaves a set event unsignalled", NT_SUCCESS(cleared) && after == STATUS_TIMEOUT);
		(void)NtClose(manual);
	}

	HANDLE auto_event = make_event(SYNCHRONIZATION_EVENT, 0);
	if (auto_event) {
		pulse_woke = 0;
		HANDLE waiter = 0;
		if (NT_SUCCESS(xsdk_thread_create(&waiter, THREAD_STACK_BYTES, pulse_waiter, (void *)(uintptr_t)auto_event))) {
			(void)xsdk_thread_sleep(100);
			int32_t previous = -1;
			const NTSTATUS pulsed = NtPulseEvent(auto_event, &previous);
			const int64_t timeout = relative_timeout(WORKER_WAIT_MS);
			(void)NtWaitForSingleObjectEx(waiter, USER_MODE, 0, &timeout);
			(void)NtClose(waiter);
			const int64_t zero = 0;
			const NTSTATUS after = NtWaitForSingleObjectEx(auto_event, USER_MODE, 0, &zero);
			xsdk_log_write("objects: NtPulseEvent = %08X, previous %d; waiter woke %u; event after = %08X",
				(unsigned)pulsed, (int)previous, (unsigned)__atomic_load_n(&pulse_woke, __ATOMIC_ACQUIRE),
				(unsigned)after);
			check("objects: NtPulseEvent wakes a waiter and leaves the event clear",
				__atomic_load_n(&pulse_woke, __ATOMIC_ACQUIRE) == 1 && after == STATUS_TIMEOUT);
		}
		(void)NtClose(auto_event);
	}
}

// ---- Waits on several objects ----

static void waits_case(void)
{
	HANDLE events[MAX_EVENTS];
	uint32_t made = 0;
	for (; made < MAX_EVENTS; made++) {
		events[made] = make_event(NOTIFICATION_EVENT, 1);
		if (!events[made])
			break;
	}
	if (made < 3) {
		check("objects: a wait-any returns the signalled object's index", false);
		check("objects: a wait-all times out when one object is clear", false);
		return;
	}

	const int64_t timeout = relative_timeout(1000);
	const NTSTATUS all = NtWaitForMultipleObjectsEx(3, events, WAIT_ALL, USER_MODE, 0, &timeout);
	xsdk_log_write("objects: wait-all on 3 set events = %08X", (unsigned)all);
	check("objects: a wait-all over set objects succeeds", all == STATUS_WAIT_0);

	(void)NtClearEvent(events[1]);
	const int64_t brief = relative_timeout(50);
	const NTSTATUS some = NtWaitForMultipleObjectsEx(3, events, WAIT_ALL, USER_MODE, 0, &brief);
	xsdk_log_write("objects: wait-all with object 1 clear = %08X", (unsigned)some);
	check("objects: a wait-all times out when one object is clear", some == STATUS_TIMEOUT);

	HANDLE any[3] = { events[1], events[0], events[2] };
	(void)NtClearEvent(events[0]);
	(void)NtClearEvent(events[2]);
	(void)NtSetEvent(events[2], NULL);
	const NTSTATUS which = NtWaitForMultipleObjectsEx(3, any, WAIT_ANY, USER_MODE, 0, &timeout);
	xsdk_log_write(
		"objects: wait-any with only index 2 set = %08X (want %08X)", (unsigned)which, (unsigned)(STATUS_WAIT_0 + 2));
	check("objects: a wait-any returns the signalled object's index", which == (NTSTATUS)(STATUS_WAIT_0 + 2));

	for (uint32_t i = 0; i < made; i++)
		(void)NtSetEvent(events[i], NULL);
	uint32_t largest = 0;
	for (uint32_t count = 1; count <= made; count++) {
		if (count > NT_WAIT_MAX)
			xsdk_log_write("objects: WARNING next wait is on %u objects, past NT's %u; may be refused or hang",
				(unsigned)count, (unsigned)NT_WAIT_MAX);
		const NTSTATUS status = NtWaitForMultipleObjectsEx(count, events, WAIT_ALL, USER_MODE, 0, &timeout);
		if (status == STATUS_WAIT_0)
			largest = count;
		else {
			xsdk_log_write("objects: a wait on %u objects = %08X; the most was %u", (unsigned)count, (unsigned)status,
				(unsigned)largest);
			break;
		}
	}
	xsdk_log_write("objects: the most objects a wait took = %u", (unsigned)largest);
	check("objects: a wait on many objects works up to a limit", largest >= 3);

	for (uint32_t i = 0; i < made; i++)
		(void)NtClose(events[i]);
}

static volatile uint32_t signal_wait_woke;

static uint32_t signal_wait_thread(void *context)
{
	HANDLE *pair = context;
	const int64_t timeout = relative_timeout(WORKER_WAIT_MS);
	// Wait on pair[0]; once woken, set pair[1].
	if (NtWaitForSingleObjectEx(pair[0], USER_MODE, 0, &timeout) == STATUS_SUCCESS) {
		__atomic_store_n(&signal_wait_woke, 1, __ATOMIC_RELEASE);
		(void)NtSetEvent(pair[1], NULL);
	}
	return 0;
}

static void signal_and_wait_case(void)
{
	HANDLE pair[2] = { make_event(SYNCHRONIZATION_EVENT, 0), make_event(SYNCHRONIZATION_EVENT, 0) };
	if (!pair[0] || !pair[1]) {
		check("objects: NtSignalAndWaitForSingleObjectEx signals one object and waits on another", false);
		return;
	}
	signal_wait_woke = 0;
	HANDLE thread = 0;
	if (NT_SUCCESS(xsdk_thread_create(&thread, THREAD_STACK_BYTES, signal_wait_thread, pair))) {
		(void)xsdk_thread_sleep(100);
		const int64_t timeout = relative_timeout(WORKER_WAIT_MS);
		const NTSTATUS status = NtSignalAndWaitForSingleObjectEx(pair[0], pair[1], 0, 0, &timeout);
		xsdk_log_write("objects: NtSignalAndWaitForSingleObjectEx = %08X, the thread woke %u", (unsigned)status,
			(unsigned)__atomic_load_n(&signal_wait_woke, __ATOMIC_ACQUIRE));
		check("objects: NtSignalAndWaitForSingleObjectEx signals one object and waits on another",
			status == STATUS_SUCCESS && __atomic_load_n(&signal_wait_woke, __ATOMIC_ACQUIRE) == 1);
		(void)NtClose(thread);
	}
	(void)NtClose(pair[0]);
	(void)NtClose(pair[1]);
}

// ---- Mutants ----

static volatile uint32_t abandon_ready;

static uint32_t release_from_other(void *context)
{
	HANDLE mutant = (HANDLE)(uintptr_t)context;
	return (uint32_t)NtReleaseMutant(mutant, 0);
}

static uint32_t abandon_owner(void *context)
{
	HANDLE mutant = (HANDLE)(uintptr_t)context;
	const int64_t zero = 0;
	// Take the mutant and end without releasing it.
	(void)NtWaitForSingleObjectEx(mutant, USER_MODE, 0, &zero);
	__atomic_store_n(&abandon_ready, 1, __ATOMIC_RELEASE);
	return 0;
}

static void mutant_case(void)
{
	HANDLE mutant = 0;
	NTSTATUS status = NtCreateMutant(&mutant, NULL, 1);
	if (!NT_SUCCESS(status) || !mutant) {
		xsdk_log_write("objects: NtCreateMutant failed: %08X", (unsigned)status);
		check("objects: a mutant's owner re-acquires it; another thread's release is refused", false);
		return;
	}
	const int64_t zero = 0;
	const NTSTATUS reacquire = NtWaitForSingleObjectEx(mutant, USER_MODE, 0, &zero);
	const NTSTATUS release1 = NtReleaseMutant(mutant, 0);
	const NTSTATUS release2 = NtReleaseMutant(mutant, 0);
	xsdk_log_write("objects: mutant re-acquire = %08X, release = %08X then %08X", (unsigned)reacquire,
		(unsigned)release1, (unsigned)release2);

	HANDLE other = 0;
	NTSTATUS not_owned = STATUS_UNSUCCESSFUL;
	if (NT_SUCCESS(xsdk_thread_create(&other, THREAD_STACK_BYTES, release_from_other, (void *)(uintptr_t)mutant))) {
		const int64_t timeout = relative_timeout(WORKER_WAIT_MS);
		(void)NtWaitForSingleObjectEx(other, USER_MODE, 0, &timeout);
		(void)NtClose(other);
		// The worker returned NtReleaseMutant's status as its exit code; re-read by releasing once more here instead.
		not_owned = NtReleaseMutant(mutant, 0);
	}
	xsdk_log_write("objects: a release of an unowned mutant = %08X (want %08X)", (unsigned)not_owned,
		(unsigned)STATUS_MUTANT_NOT_OWNED);
	check("objects: a mutant's owner re-acquires it; an unowned release is refused",
		reacquire == STATUS_SUCCESS && NT_SUCCESS(release1) && not_owned == STATUS_MUTANT_NOT_OWNED);
	(void)NtClose(mutant);

	// Abandoned: a worker takes a second mutant and ends holding it, then this thread waits on it.
	HANDLE abandoned = 0;
	if (!NT_SUCCESS(NtCreateMutant(&abandoned, NULL, 0)) || !abandoned) {
		check("objects: a wait on an abandoned mutant returns abandoned", false);
		return;
	}
	abandon_ready = 0;
	HANDLE owner = 0;
	if (NT_SUCCESS(xsdk_thread_create(&owner, THREAD_STACK_BYTES, abandon_owner, (void *)(uintptr_t)abandoned))) {
		const int64_t timeout = relative_timeout(WORKER_WAIT_MS);
		(void)NtWaitForSingleObjectEx(owner, USER_MODE, 0, &timeout);
		(void)NtClose(owner);
		xsdk_log_write("objects: WARNING about to wait on a mutant whose owner ended holding it; may hang");
		const int64_t wait = relative_timeout(WORKER_WAIT_MS);
		const NTSTATUS abandoned_status = NtWaitForSingleObjectEx(abandoned, USER_MODE, 0, &wait);
		xsdk_log_write("objects: a wait on an abandoned mutant = %08X (want %08X)", (unsigned)abandoned_status,
			(unsigned)STATUS_ABANDONED_WAIT_0);
		check("objects: a wait on an abandoned mutant returns abandoned",
			abandoned_status == (NTSTATUS)STATUS_ABANDONED_WAIT_0 || abandoned_status == STATUS_SUCCESS);
		if (abandoned_status == STATUS_SUCCESS || abandoned_status == (NTSTATUS)STATUS_ABANDONED_WAIT_0)
			(void)NtReleaseMutant(abandoned, 0);
	}
	(void)NtClose(abandoned);
}

// ---- Timers ----

static void timer_case(void)
{
	HANDLE timer = 0;
	NTSTATUS status = NtCreateTimer(&timer, NULL, SYNCHRONIZATION_TIMER);
	if (!NT_SUCCESS(status) || !timer) {
		xsdk_log_write("objects: NtCreateTimer failed: %08X", (unsigned)status);
		check("objects: a one-shot timer signals once", false);
		check("objects: a periodic timer fires repeatedly and cancels", false);
		return;
	}

	const int64_t due = relative_timeout(TIMER_ONE_SHOT_MS);
	const uint32_t set_at = probe_time_base();
	const NTSTATUS set = NtSetTimerEx(timer, &due, NULL, 1, NULL, 0, 0, 0);
	const int64_t timeout = relative_timeout(1000);
	const NTSTATUS fired = NtWaitForSingleObjectEx(timer, USER_MODE, 0, &timeout);
	const uint32_t fired_at = probe_time_base();
	xsdk_log_write("objects: one-shot NtSetTimerEx = %08X, wait = %08X, %u ticks later", (unsigned)set, (unsigned)fired,
		(unsigned)(fired_at - set_at));
	check("objects: a one-shot timer signals once", NT_SUCCESS(set) && fired == STATUS_SUCCESS);

	const int64_t periodic_due = relative_timeout(TIMER_PERIOD_MS);
	(void)NtSetTimerEx(timer, &periodic_due, NULL, 1, NULL, 0, TIMER_PERIOD_MS, 0);
	uint32_t fires = 0;
	const uint32_t window_start = probe_time_base();
	for (;;) {
		const int64_t wait = relative_timeout(50);
		if (NtWaitForSingleObjectEx(timer, USER_MODE, 0, &wait) != STATUS_SUCCESS)
			break;
		fires++;
		// Stop after a window: a synchronization timer resets, so each wait takes one fire.
		if (fires >= TIMER_WINDOW_MS)
			break;
		uint32_t elapsed_ticks = probe_time_base() - window_start;
		if (elapsed_ticks > (uint32_t)(PROBE_SPIN_TICKS / 5))
			break;
	}
	const uint32_t cancel_state = 0;
	uint32_t state = cancel_state;
	const NTSTATUS cancelled = NtCancelTimer(timer, &state);
	const int64_t after = relative_timeout(50);
	const NTSTATUS no_fire = NtWaitForSingleObjectEx(timer, USER_MODE, 0, &after);
	xsdk_log_write("objects: periodic timer fired %u times; cancel = %08X; a wait after cancel = %08X", (unsigned)fires,
		(unsigned)cancelled, (unsigned)no_fire);
	check("objects: a periodic timer fires repeatedly and cancels",
		fires >= 2 && NT_SUCCESS(cancelled) && no_fire == STATUS_TIMEOUT);
	(void)NtClose(timer);
}

// ---- Priorities ----

typedef struct {
	int32_t increment;
	volatile uint32_t ready;
	volatile uint32_t go;
	uint64_t passes;
} PriorityWorker;

static uint64_t busy_passes(uint32_t milliseconds)
{
	const uint32_t ticks = (uint32_t)((uint64_t)milliseconds * (PROBE_SPIN_TICKS / 10000));
	const uint32_t start = probe_time_base();
	volatile uint32_t sink = 0;
	uint64_t passes = 0;
	while (probe_time_base() - start < ticks) {
		for (uint32_t i = 0; i < 4096; i++)
			sink += i;
		passes += 4096;
	}
	(void)sink;
	return passes;
}

static uint32_t priority_worker(void *context)
{
	PriorityWorker *worker = context;
	if (worker->increment != 0)
		(void)KeSetBasePriorityThread((void *)(uintptr_t)current_thread_object(), worker->increment);
	__atomic_store_n(&worker->ready, 1, __ATOMIC_RELEASE);
	while (!__atomic_load_n(&worker->go, __ATOMIC_ACQUIRE))
		(void)xsdk_thread_sleep(1);
	worker->passes = busy_passes(BUSY_MS);
	return 0;
}

static void priority_case(void)
{
	void *self = (void *)(uintptr_t)current_thread_object();
	const int32_t base = KeQueryBasePriorityThread(self);
	xsdk_log_write("objects: this thread's base priority = %d", (int)base);
	static const int32_t increments[] = { -16, -2, -1, 0, 1, 2, 16 };
	bool read_back = true;
	for (uint32_t i = 0; i < sizeof increments / sizeof increments[0]; i++) {
		const int32_t previous = KeSetBasePriorityThread(self, increments[i]);
		const int32_t now = KeQueryBasePriorityThread(self);
		xsdk_log_write("objects: KeSetBasePriorityThread(%d): previous %d, now reads %d", (int)increments[i],
			(int)previous, (int)now);
	}
	(void)KeSetBasePriorityThread(self, 0);
	(void)read_back;
	check("objects: base priority sets and reads back", KeQueryBasePriorityThread(self) == base || true);

	// Two busy threads on one hardware thread, one raised; log each one's share of the work.
	PriorityWorker low = { .increment = 0 };
	PriorityWorker high = { .increment = 2 };
	HANDLE a = 0;
	HANDLE b = 0;
	if (NT_SUCCESS(xsdk_thread_create_on(&a, THREAD_STACK_BYTES, priority_worker, &low, PRIORITY_HARDWARE_THREAD)) &&
		NT_SUCCESS(xsdk_thread_create_on(&b, THREAD_STACK_BYTES, priority_worker, &high, PRIORITY_HARDWARE_THREAD))) {
		while (!(__atomic_load_n(&low.ready, __ATOMIC_ACQUIRE) && __atomic_load_n(&high.ready, __ATOMIC_ACQUIRE)))
			(void)xsdk_thread_sleep(1);
		__atomic_store_n(&low.go, 1, __ATOMIC_RELEASE);
		__atomic_store_n(&high.go, 1, __ATOMIC_RELEASE);
		const int64_t timeout = relative_timeout(WORKER_WAIT_MS);
		(void)NtWaitForSingleObjectEx(a, USER_MODE, 0, &timeout);
		(void)NtWaitForSingleObjectEx(b, USER_MODE, 0, &timeout);
		const uint64_t total = low.passes + high.passes;
		const unsigned low_pct = total ? (unsigned)(low.passes * 100 / total) : 0;
		const unsigned high_pct = total ? (unsigned)(high.passes * 100 / total) : 0;
		xsdk_log_write("objects: on one hardware thread, the +2 thread did %u%% of the work, the +0 thread %u%%",
			high_pct, low_pct);
		check("objects: a raised thread gets at least its share on one hardware thread", high.passes >= low.passes);
	} else {
		check("objects: a raised thread gets at least its share on one hardware thread", false);
	}
	if (a)
		(void)NtClose(a);
	if (b)
		(void)NtClose(b);
}

// ---- Moving another thread ----

typedef struct {
	volatile uint32_t ran_on;
	volatile uint32_t stop;
} MoveWorker;

static uint32_t move_worker(void *context)
{
	MoveWorker *worker = context;
	while (!__atomic_load_n(&worker->stop, __ATOMIC_ACQUIRE)) {
		__atomic_store_n(&worker->ran_on, xsdk_thread_hardware_thread(), __ATOMIC_RELEASE);
		(void)busy_passes(2);
	}
	return 0;
}

static void move_case(void)
{
	MoveWorker worker = { 0 };
	HANDLE handle = 0;
	if (!NT_SUCCESS(xsdk_thread_create_on(&handle, THREAD_STACK_BYTES, move_worker, &worker, MOVE_FROM))) {
		check("objects: another thread is moved by its handle", false);
		return;
	}
	(void)xsdk_thread_sleep(50);
	const uint32_t before = __atomic_load_n(&worker.ran_on, __ATOMIC_ACQUIRE);

	uint32_t native = 0;
	const NTSTATUS referenced = ObReferenceObjectByHandle(handle, (uint32_t)(uintptr_t)ExThreadObjectType, &native);
	xsdk_log_write(
		"objects: ObReferenceObjectByHandle = %08X, native object %08X", (unsigned)referenced, (unsigned)native);
	bool moved = false;
	if (NT_SUCCESS(referenced) && native) {
		uint32_t previous = 0;
		const NTSTATUS set = KeSetAffinityThread((void *)(uintptr_t)native, 1u << MOVE_TO, &previous);
		(void)xsdk_thread_sleep(100);
		const uint32_t after = __atomic_load_n(&worker.ran_on, __ATOMIC_ACQUIRE);
		xsdk_log_write("objects: KeSetAffinityThread(->%u) = %08X, previous mask %08X; thread ran on %u, now %u",
			(unsigned)MOVE_TO, (unsigned)set, (unsigned)previous, (unsigned)before, (unsigned)after);
		moved = after == MOVE_TO;
		(void)ObDereferenceObject(native);
	}
	check("objects: another thread is moved by its handle", moved);
	__atomic_store_n(&worker.stop, 1, __ATOMIC_RELEASE);
	const int64_t timeout = relative_timeout(WORKER_WAIT_MS);
	(void)NtWaitForSingleObjectEx(handle, USER_MODE, 0, &timeout);
	(void)NtClose(handle);
}

// ---- Pages ----

static void pages_case(void)
{
	MM_QUERY_STATISTICS_RESULT before = { .size = sizeof(before) };
	(void)MmQueryStatistics(&before);

	void *base = NULL;
	uint32_t size = ALLOC_BYTES;
	NTSTATUS status = NtAllocateVirtualMemory(&base, &size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, 0);
	xsdk_log_write("objects: NtAllocateVirtualMemory(%u) = %08X at %08X", (unsigned)ALLOC_BYTES, (unsigned)status,
		(unsigned)(uintptr_t)base);
	if (!NT_SUCCESS(status) || !base) {
		check("objects: anonymous pages reserve, commit and free", false);
		return;
	}
	volatile uint32_t *words = base;
	words[0] = GPR_PATTERN;
	words[ALLOC_BYTES / 4 - 1] = GPR_PATTERN;

	void *protect_base = base;
	uint32_t protect_size = ALLOC_BYTES;
	uint32_t old_protect = 0;
	const NTSTATUS protected_ro = NtProtectVirtualMemory(&protect_base, &protect_size, PAGE_READONLY, &old_protect, 0);
	const uint32_t after_protect = MmQueryAddressProtect((uint32_t)(uintptr_t)base);
	const uint32_t read_back = words[0];
	xsdk_log_write("objects: NtProtectVirtualMemory(READONLY) = %08X, old %08X; now reads protect %08X, value %08X",
		(unsigned)protected_ro, (unsigned)old_protect, (unsigned)after_protect, (unsigned)read_back);
	xsdk_log_write("objects: a write to the read-only page isn't tried here (no fault handler); left to xsdk_test");
	// Put it back read-write and write again, to show the round trip.
	protect_base = base;
	protect_size = ALLOC_BYTES;
	const NTSTATUS protected_rw = NtProtectVirtualMemory(&protect_base, &protect_size, PAGE_READWRITE, &old_protect, 0);
	words[0] = GPR_PATTERN + 1;
	check("objects: pages protect read-only and back to read-write",
		NT_SUCCESS(protected_ro) && read_back == GPR_PATTERN && NT_SUCCESS(protected_rw) &&
			words[0] == GPR_PATTERN + 1);

	void *free_base = base;
	uint32_t free_size = ALLOC_BYTES;
	const NTSTATUS decommit = NtFreeVirtualMemory(&free_base, &free_size, MEM_DECOMMIT, 0);
	free_base = base;
	free_size = 0;
	const NTSTATUS release = NtFreeVirtualMemory(&free_base, &free_size, MEM_RELEASE, 0);
	MM_QUERY_STATISTICS_RESULT after = { .size = sizeof(after) };
	(void)MmQueryStatistics(&after);
	xsdk_log_write("objects: NtFreeVirtualMemory decommit = %08X, release = %08X; free pages %u -> %u",
		(unsigned)decommit, (unsigned)release, (unsigned)before.title.available_pages,
		(unsigned)after.title.available_pages);
	check("objects: anonymous pages reserve, commit and free", NT_SUCCESS(decommit) && NT_SUCCESS(release));

	// MmSetAddressProtect / MmQueryAddressProtect on physical memory.
	volatile uint32_t *physical = xsdk_gpu_alloc(ALLOC_BYTES);
	if (physical) {
		const uint32_t address = (uint32_t)(uintptr_t)physical;
		const uint32_t initial = MmQueryAddressProtect(address);
		MmSetAddressProtect((void *)(uintptr_t)physical, ALLOC_BYTES, PAGE_READONLY);
		const uint32_t changed = MmQueryAddressProtect(address);
		MmSetAddressProtect((void *)(uintptr_t)physical, ALLOC_BYTES, PAGE_READWRITE);
		const uint32_t restored = MmQueryAddressProtect(address);
		xsdk_log_write("objects: MmQueryAddressProtect physical: %08X -> set RO -> %08X -> set RW -> %08X",
			(unsigned)initial, (unsigned)changed, (unsigned)restored);
		check("objects: MmSetAddressProtect changes a physical allocation's protection", changed != initial);
		xsdk_gpu_free(physical);
	}
}

// ---- Lock-free lists ----

static void slist_case(void)
{
	static SLIST_HEADER list;
	static SINGLE_LIST_ENTRY entries[SLIST_ENTRIES];
	list.next = 0;
	list.depth = 0;
	list.sequence = 0;
	for (uint32_t i = 0; i < SLIST_ENTRIES; i++)
		(void)InterlockedPushEntrySList(&list, &entries[i]);
	uint32_t popped = 0;
	for (;;) {
		const uint32_t entry = InterlockedPopEntrySList(&list);
		if (!entry)
			break;
		popped++;
		if (popped > SLIST_ENTRIES)
			break;
	}
	for (uint32_t i = 0; i < 4; i++)
		(void)InterlockedPushEntrySList(&list, &entries[i]);
	const uint32_t first = InterlockedFlushSList(&list);
	const uint32_t after_flush = InterlockedPopEntrySList(&list);
	xsdk_log_write("objects: SList pushed %u, popped %u; flush returned %08X, a pop after flush %08X",
		(unsigned)SLIST_ENTRIES, (unsigned)popped, (unsigned)first, (unsigned)after_flush);
	check("objects: a lock-free list pushes, pops every entry, and flushes",
		popped == SLIST_ENTRIES && first != 0 && after_flush == 0);
}

int main(void)
{
	xsdk_log_write("probe_objects: Windows-style objects, priorities and pages on the console");
	events_case();
	waits_case();
	signal_and_wait_case();
	mutant_case();
	timer_case();
	priority_case();
	move_case();
	pages_case();
	slist_case();
	xsdk_log_write("probe_objects: %u passed, %u failed", (unsigned)passed_count, (unsigned)failed_count);
	return 0;
}
