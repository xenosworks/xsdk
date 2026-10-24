#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "probe.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Times the memory accesses a JIT needs to run little-endian guest code on this big-endian host, each beside its plain
// form: byte-reversed loads and stores, and what takes their place where the FPRs and VMX have none. What it finds
// decides how the JIT reaches guest memory (MSR[LE] through the hypervisor is ruled out for xsdk). An operation is one
// run of the sequence in the test's name; each kernel, in tests/probe_byterev_kernels.S, does 8 of them a pass.

enum {
	ROUNDS = 9,
	// The time base ticks every 64 CPU clocks: 3192 MHz over 49.875 MHz (src/time.c).
	TIME_BASE_HZ = 49875000,
	CLOCKS_PER_TICK = 64,
	// A run takes about 40 ms; a short run of the kernel first says how many passes that is.
	TARGET_TICKS = 2000000,
	CALIBRATION_PASSES = 4096,
	OPERATIONS_PER_PASS = 8,
	// The kernels' eight slots, 144 bytes apart as in the kernels.
	SLOTS = 8,
	SLOT_STRIDE_BYTES = 144,
	BUFFER_BYTES = 2048,
	VALUE_COUNT = 16,
	// A chase steps 17 entries each time, which visits all of them, as 17 is odd and the counts are powers of two.
	CHASE_STRIDE = 17,
	MAIN_HARDWARE_THREAD = 0,
};

// How a kernel's result is prepared and checked.
typedef enum {
	LOAD,
	CHASE,
	STORE,
	STORE_THEN_LOAD,
} Kind;

// Which of a kernel's accesses reverse bytes.
enum { LOAD_REVERSED = 1, STORE_REVERSED = 2 };

typedef void Kernel(void *base, uint32_t passes, uint64_t *values);

typedef struct {
	const char *name;
	Kernel *run;
	Kind kind;
	uint32_t size_bytes;
	uint32_t reversed;
} Test;

// tests/probe_byterev_kernels.S
Kernel probe_byterev_lhzx, probe_byterev_lhbrx, probe_byterev_lwzx, probe_byterev_lwbrx, probe_byterev_ldx,
	probe_byterev_ldbrx, probe_byterev_chase_lwzx, probe_byterev_chase_lwbrx, probe_byterev_chase_ldx,
	probe_byterev_chase_ldbrx, probe_byterev_sthx, probe_byterev_sthbrx, probe_byterev_stwx, probe_byterev_stwbrx,
	probe_byterev_stdx, probe_byterev_stdbrx, probe_byterev_stwx_lwzx, probe_byterev_stwbrx_lwzx,
	probe_byterev_stwbrx_lwbrx;

static const Test tests[] = {
	{ "load: lhzx", probe_byterev_lhzx, LOAD, 2, 0 },
	{ "load: lhbrx", probe_byterev_lhbrx, LOAD, 2, LOAD_REVERSED },
	{ "load: lwzx", probe_byterev_lwzx, LOAD, 4, 0 },
	{ "load: lwbrx", probe_byterev_lwbrx, LOAD, 4, LOAD_REVERSED },
	{ "load: ldx", probe_byterev_ldx, LOAD, 8, 0 },
	{ "chase: lwzx", probe_byterev_chase_lwzx, CHASE, 4, 0 },
	{ "chase: lwbrx", probe_byterev_chase_lwbrx, CHASE, 4, LOAD_REVERSED },
	{ "chase: ldx", probe_byterev_chase_ldx, CHASE, 8, 0 },
	{ "store: sthx", probe_byterev_sthx, STORE, 2, 0 },
	{ "store: sthbrx", probe_byterev_sthbrx, STORE, 2, STORE_REVERSED },
	{ "store: stwx", probe_byterev_stwx, STORE, 4, 0 },
	{ "store: stwbrx", probe_byterev_stwbrx, STORE, 4, STORE_REVERSED },
	{ "store: stdx", probe_byterev_stdx, STORE, 8, 0 },
	{ "store then load: stwx; lwzx", probe_byterev_stwx_lwzx, STORE_THEN_LOAD, 4, 0 },
	{ "store then load: stwbrx; lwzx", probe_byterev_stwbrx_lwzx, STORE_THEN_LOAD, 4, STORE_REVERSED },
	{ "store then load: stwbrx; lwbrx", probe_byterev_stwbrx_lwbrx, STORE_THEN_LOAD, 4,
		STORE_REVERSED | LOAD_REVERSED },
};

// ldbrx and stdbrx, which the console may refuse, so they run after the rest.
static const Test last_tests[] = {
	{ "load: ldbrx", probe_byterev_ldbrx, LOAD, 8, LOAD_REVERSED },
	{ "chase: ldbrx", probe_byterev_chase_ldbrx, CHASE, 8, LOAD_REVERSED },
	{ "store: stdbrx", probe_byterev_stdbrx, STORE, 8, STORE_REVERSED },
};

enum {
	TESTS = sizeof tests / sizeof tests[0],
	LAST_TESTS = sizeof last_tests / sizeof last_tests[0],
};

static uint8_t buffer[BUFFER_BYTES] __attribute__((aligned(128)));
static uint64_t values[VALUE_COUNT] __attribute__((aligned(16)));

// The byte a test puts at index i of slot: distinct within a slot and across slots.
static uint8_t pattern_byte(uint32_t slot, uint32_t i)
{
	return (uint8_t)(16 * slot + i + 1);
}

// The bytes in memory that a store of the low size_bytes of value leaves, a byte-reversing store's reversed.
static void store_image(uint8_t *bytes, uint64_t value, uint32_t size_bytes, bool reversed)
{
	for (uint32_t i = 0; i < size_bytes; i++)
		bytes[i] = (uint8_t)(value >> 8 * (reversed ? i : size_bytes - 1 - i));
}

// What a load of size_bytes bytes gives from them, a byte-reversing load's reversed.
static uint64_t load_image(const uint8_t *bytes, uint32_t size_bytes, bool reversed)
{
	uint64_t value = 0;
	for (uint32_t i = 0; i < size_bytes; i++)
		value = value << 8 | bytes[reversed ? size_bytes - 1 - i : i];
	return value;
}

// What a load of the slot's pattern gives.
static uint64_t slot_value(uint32_t slot, uint32_t size_bytes, bool reversed)
{
	uint8_t bytes[8];
	for (uint32_t i = 0; i < size_bytes; i++)
		bytes[i] = pattern_byte(slot, i);
	return load_image(bytes, size_bytes, reversed);
}

// Each entry holds the offset of the one 17 on.
static void fill_chain(const Test *test)
{
	const uint32_t entries = BUFFER_BYTES / test->size_bytes;
	for (uint32_t i = 0; i < entries; i++) {
		const uint64_t next = (uint64_t)((i + CHASE_STRIDE) % entries) * test->size_bytes;
		store_image(buffer + i * test->size_bytes, next, test->size_bytes, test->reversed & LOAD_REVERSED);
	}
}

static void prepare(const Test *test)
{
	memset(buffer, 0, sizeof buffer);
	memset(values, 0, sizeof values);
	switch (test->kind) {
	case LOAD:
		for (uint32_t slot = 0; slot < SLOTS; slot++) {
			for (uint32_t i = 0; i < 16; i++)
				buffer[slot * SLOT_STRIDE_BYTES + i] = pattern_byte(slot, i);
		}
		break;
	case CHASE:
		fill_chain(test);
		break;
	case STORE:
		for (uint32_t slot = 0; slot < SLOTS; slot++)
			values[slot] = slot_value(slot, 8, false);
		break;
	case STORE_THEN_LOAD:
		values[0] = slot_value(0, 4, false);
		break;
	}
}

// Whether what the kernel left is what the instructions it names must leave, byte order and all.
static bool check(const Test *test, uint32_t passes)
{
	const uint32_t size_bytes = test->size_bytes;
	const bool load_reversed = test->reversed & LOAD_REVERSED;
	const bool store_reversed = test->reversed & STORE_REVERSED;
	bool ok = true;
	switch (test->kind) {
	case LOAD:
		for (uint32_t slot = 0; slot < SLOTS; slot++)
			ok = ok && values[slot] == slot_value(slot, size_bytes, load_reversed);
		break;
	case CHASE: {
		const uint64_t entries = BUFFER_BYTES / size_bytes;
		const uint64_t steps = (uint64_t)passes * OPERATIONS_PER_PASS;
		ok = values[0] == steps * CHASE_STRIDE % entries * size_bytes;
		break;
	}
	case STORE:
		for (uint32_t slot = 0; slot < SLOTS; slot++) {
			uint8_t expected[8] = { 0 };
			store_image(expected, values[slot], size_bytes, store_reversed);
			ok = ok && memcmp(buffer + slot * SLOT_STRIDE_BYTES, expected, sizeof expected) == 0;
		}
		break;
	case STORE_THEN_LOAD: {
		// The kernel does an odd count of pairs, so the word was last stored as it began, and the register holds the
		// load of that.
		uint8_t image[4];
		store_image(image, slot_value(0, 4, false), sizeof image, store_reversed);
		ok = values[0] == load_image(image, sizeof image, load_reversed) && memcmp(buffer, image, sizeof image) == 0;
		break;
	}
	}
	return ok;
}

// Prepares, times the kernel for passes passes, and checks what it left; false if the check fails.
static bool run_once(const Test *test, uint32_t passes, uint32_t *ticks)
{
	prepare(test);
	const uint32_t start = probe_time_base();
	test->run(buffer, passes, values);
	*ticks = probe_time_base() - start;
	return check(test, passes);
}

// The passes that make a run last TARGET_TICKS, from the time of a short one.
static uint32_t passes_for_target(const Test *test)
{
	uint32_t ticks;
	(void)run_once(test, CALIBRATION_PASSES, &ticks);
	const uint64_t passes = (uint64_t)CALIBRATION_PASSES * TARGET_TICKS / (ticks + 1);
	return passes ? (uint32_t)passes : 1;
}

static void sort_doubles(double *items, uint32_t count)
{
	for (uint32_t i = 1; i < count; i++) {
		const double item = items[i];
		uint32_t j = i;
		for (; j > 0 && items[j - 1] > item; j--)
			items[j] = items[j - 1];
		items[j] = item;
	}
}

// Times the test ROUNDS times and logs its clocks an operation; false if any check failed.
static bool measure(const Test *test)
{
	// Flushed to the log before the first run, so that an instruction the console refuses leaves the test's name.
	xsdk_log_write("BEGIN %s", test->name);
	const uint32_t passes = passes_for_target(test);
	const double operations = (double)passes * OPERATIONS_PER_PASS;
	double clocks[ROUNDS];
	bool ok = true;
	for (uint32_t round = 0; round < ROUNDS; round++) {
		uint32_t ticks;
		ok = run_once(test, passes, &ticks) && ok;
		clocks[round] = (double)ticks * CLOCKS_PER_TICK / operations;
	}
	sort_doubles(clocks, ROUNDS);
	xsdk_log_write(
		"%s: byterev %s: %.1f clocks an operation at best, %.1f the median, %.1f the worst, %.0f operations a run",
		ok ? "PASS" : "FAIL", test->name, clocks[0], clocks[ROUNDS / 2], clocks[ROUNDS - 1], operations);
	return ok;
}

static uint32_t measure_all(const Test *list, uint32_t count)
{
	uint32_t failed = 0;
	for (uint32_t i = 0; i < count; i++)
		failed += !measure(&list[i]);
	return failed;
}

int main(void)
{
	uint32_t failed = 0;
	const NTSTATUS moved = xsdk_thread_move(MAIN_HARDWARE_THREAD);
	const uint32_t hardware_thread = xsdk_thread_hardware_thread();
	xsdk_log_write("%s: byterev: xsdk_thread_move(%u) returned 0x%08X, the main thread runs on hardware thread %u",
		NT_SUCCESS(moved) && hardware_thread == MAIN_HARDWARE_THREAD ? "PASS" : "FAIL", (unsigned)MAIN_HARDWARE_THREAD,
		(unsigned)moved, (unsigned)hardware_thread);
	failed += !(NT_SUCCESS(moved) && hardware_thread == MAIN_HARDWARE_THREAD);
	xsdk_log_write("byterev: %u rounds a test, runs of about %u ms", (unsigned)ROUNDS,
		(unsigned)((uint64_t)TARGET_TICKS * 1000 / TIME_BASE_HZ));
	failed += measure_all(tests, TESTS);
	failed += measure_all(last_tests, LAST_TESTS);
	xsdk_log_write("TESTS COMPLETE: status=%d", failed ? 1 : 0);
	return failed ? 1 : 0;
}
