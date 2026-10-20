#include <stdbool.h>
#include <stdint.h>

#include "probe.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Milestone 52 step 1's probe (docs/scope/52-compiled-cpu-code.md): what an atomic increment costs at each memory
// order, lwsync against sync, and two threads incrementing one counter on two cores and on one.

enum {
	INCREMENTS = 1000000,
	ROUNDS = 5,
	// The time base ticks every 64 CPU clocks: 3192 MHz over 49.875 MHz (src/time.c).
	TIME_BASE_HZ = 49875000,
	CLOCKS_PER_TICK = 64,
	THREAD_STACK_BYTES = 64 * 1024,
	MAIN_HARDWARE_THREAD = 0,
	// Hardware threads 2 and 3 share a core, 2 and 4 don't (include/xsdk/thread.h).
	FIRST_WORKER = 2,
	SAME_CORE_WORKER = 3,
	OTHER_CORE_WORKER = 4,
	WORKERS = 2,
};

// Five seconds, relative, in 100 ns units.
static const int64_t WAIT_TIMEOUT = -50000000;

typedef uint32_t (*Run)(void);

typedef struct {
	const char *name;
	Run run;
} Loop;

// A 128-byte cache line of its own.
static uint32_t counter __attribute__((aligned(128)));
static volatile uint32_t plain_counter __attribute__((aligned(128)));

// Each loop returns the time base ticks it took for INCREMENTS passes.
static inline __attribute__((always_inline)) uint32_t time_atomic(int order)
{
	const uint32_t start = probe_time_base();
	for (uint32_t i = 0; i < INCREMENTS; i++)
		__atomic_fetch_add(&counter, 1, order);
	return probe_time_base() - start;
}

static uint32_t time_empty(void)
{
	const uint32_t start = probe_time_base();
	for (uint32_t i = 0; i < INCREMENTS; i++)
		__asm__ volatile("");
	return probe_time_base() - start;
}

static uint32_t time_plain(void)
{
	const uint32_t start = probe_time_base();
	for (uint32_t i = 0; i < INCREMENTS; i++)
		plain_counter++;
	return probe_time_base() - start;
}

static uint32_t time_relaxed(void)
{
	return time_atomic(__ATOMIC_RELAXED);
}

static uint32_t time_acquire(void)
{
	return time_atomic(__ATOMIC_ACQUIRE);
}

static uint32_t time_release(void)
{
	return time_atomic(__ATOMIC_RELEASE);
}

static uint32_t time_acq_rel(void)
{
	return time_atomic(__ATOMIC_ACQ_REL);
}

static uint32_t time_seq_cst(void)
{
	return time_atomic(__ATOMIC_SEQ_CST);
}

static uint32_t time_lwsync(void)
{
	const uint32_t start = probe_time_base();
	for (uint32_t i = 0; i < INCREMENTS; i++)
		__asm__ volatile("lwsync" : : : "memory");
	return probe_time_base() - start;
}

static uint32_t time_sync(void)
{
	const uint32_t start = probe_time_base();
	for (uint32_t i = 0; i < INCREMENTS; i++)
		__asm__ volatile("sync" : : : "memory");
	return probe_time_base() - start;
}

static uint32_t time_isync(void)
{
	const uint32_t start = probe_time_base();
	for (uint32_t i = 0; i < INCREMENTS; i++)
		__asm__ volatile("isync" : : : "memory");
	return probe_time_base() - start;
}

static const Loop alone[] = {
	{ "empty loop", time_empty },
	{ "plain increment", time_plain },
	{ "relaxed", time_relaxed },
	{ "acquire", time_acquire },
	{ "release", time_release },
	{ "acq_rel", time_acq_rel },
	{ "seq_cst", time_seq_cst },
	{ "lwsync", time_lwsync },
	{ "sync", time_sync },
	{ "isync", time_isync },
};

static const Loop contended[] = {
	{ "relaxed", time_relaxed },
	{ "seq_cst", time_seq_cst },
};

enum {
	ALONE = sizeof alone / sizeof alone[0],
	CONTENDED = sizeof contended / sizeof contended[0],
};

typedef struct {
	Run run;
	uint32_t ticks;
} Worker;

static volatile uint32_t ready;
static volatile uint32_t go;

static double ns_each(uint32_t ticks)
{
	return (double)ticks * 1e9 / TIME_BASE_HZ / INCREMENTS;
}

static double clocks_each(uint32_t ticks)
{
	return (double)ticks * CLOCKS_PER_TICK / INCREMENTS;
}

static void time_alone(const Loop *loop)
{
	uint32_t best = UINT32_MAX;
	uint32_t worst = 0;
	for (uint32_t round = 0; round < ROUNDS; round++) {
		const uint32_t ticks = loop->run();
		best = ticks < best ? ticks : best;
		worst = ticks > worst ? ticks : worst;
	}
	xsdk_log_write("atomics probe: alone, %s: %.2f ns (%.1f clocks) a pass at best, %.2f ns at worst", loop->name,
		ns_each(best), clocks_each(best), ns_each(worst));
}

// Both workers spin until go, so they start together; the main thread sleeps on its own hardware thread meanwhile.
static uint32_t contend(void *context)
{
	Worker *const worker = context;
	__atomic_add_fetch(&ready, 1, __ATOMIC_SEQ_CST);
	while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE)) {
	}
	worker->ticks = worker->run();
	return 0;
}

// Runs loop on two hardware threads at once; false if a thread failed or the count came out wrong.
static bool time_contended(const Loop *loop, uint32_t second_hardware_thread, const char *where)
{
	const uint32_t hardware_threads[WORKERS] = { FIRST_WORKER, second_hardware_thread };
	Worker workers[WORKERS] = { { loop->run, 0 }, { loop->run, 0 } };
	HANDLE handles[WORKERS];
	counter = 0;
	ready = 0;
	go = 0;
	uint32_t started = 0;
	for (; started < WORKERS; started++) {
		const NTSTATUS status = xsdk_thread_create_on(
			&handles[started], THREAD_STACK_BYTES, contend, &workers[started], hardware_threads[started]);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("atomics probe: xsdk_thread_create_on(%u) failed: 0x%08X",
				(unsigned)hardware_threads[started], (unsigned)status);
			break;
		}
	}
	while (ready < started)
		(void)xsdk_thread_sleep(1);
	go = 1;
	bool ok = started == WORKERS;
	for (uint32_t i = 0; i < started; i++) {
		const NTSTATUS status = NtWaitForSingleObjectEx(handles[i], USER_MODE, 0, &WAIT_TIMEOUT);
		if (status != STATUS_SUCCESS) {
			xsdk_log_write("atomics probe: NtWaitForSingleObjectEx failed: 0x%08X", (unsigned)status);
			ok = false;
		}
		(void)NtClose(handles[i]);
	}
	const uint32_t count = __atomic_load_n(&counter, __ATOMIC_SEQ_CST);
	ok = ok && count == WORKERS * INCREMENTS;
	xsdk_log_write(
		"atomics probe: two threads %s, %s: %.2f and %.2f ns (%.1f and %.1f clocks) an increment, "
		"count %u of %u",
		where, loop->name, ns_each(workers[0].ticks), ns_each(workers[1].ticks), clocks_each(workers[0].ticks),
		clocks_each(workers[1].ticks), (unsigned)count, (unsigned)(WORKERS * INCREMENTS));
	return ok;
}

int main(void)
{
	const NTSTATUS status = xsdk_thread_move(MAIN_HARDWARE_THREAD);
	if (!NT_SUCCESS(status))
		xsdk_log_write(
			"atomics probe: xsdk_thread_move(%u) failed: 0x%08X", (unsigned)MAIN_HARDWARE_THREAD, (unsigned)status);
	xsdk_log_write("atomics probe: %u passes a loop, best and worst of %u rounds, on hardware thread %u",
		(unsigned)INCREMENTS, (unsigned)ROUNDS, (unsigned)xsdk_thread_hardware_thread());
	for (uint32_t i = 0; i < ALONE; i++)
		time_alone(&alone[i]);
	bool ok = true;
	for (uint32_t i = 0; i < CONTENDED; i++) {
		ok = time_contended(&contended[i], OTHER_CORE_WORKER, "on two cores (2 and 4)") && ok;
		ok = time_contended(&contended[i], SAME_CORE_WORKER, "on one core (2 and 3)") && ok;
	}
	xsdk_log_write("atomics probe: %s", ok ? "every contended count came out right" : "a contended run failed");
	return ok ? 0 : 1;
}
