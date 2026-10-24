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
	// The main thread's hardware thread, and its sibling's on the same core (include/xsdk/thread.h).
	MAIN_HARDWARE_THREAD = 0,
	SIBLING_HARDWARE_THREAD = 1,
	// The sibling's spinner counts its passes of 8 adds in blocks of this many.
	SPIN_PASSES = 1 << 14,
	SPINNER_STACK_BYTES = 64 * 1024,
	SPINNER_START_MS = 1000,
	IDLE_MS = 40,
	// The first addend of an add kernel; the others follow it.
	ADDEND = 3,
};

// How a kernel's result is prepared and checked.
typedef enum {
	LOAD,
	CHASE,
	STORE,
	STORE_THEN_LOAD,
	FLOAT_LOAD,
	FLOAT_CHASE,
	FLOAT_STORE,
	VECTOR_LOAD,
	VECTOR_STORE,
	ADD_CHAIN,
	ADD_STREAM,
} Kind;

// Which of a kernel's accesses reverse bytes.
enum { LOAD_REVERSED = 1, STORE_REVERSED = 2 };

typedef void Kernel(void *base, uint32_t passes, uint64_t *values);

// size_bytes is the width of the access, or for a vector the bytes that vperm reverses together.
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
	probe_byterev_stwbrx_lwbrx, probe_byterev_lfsx, probe_byterev_lwbrx_lfs, probe_byterev_chase_lfsx,
	probe_byterev_chase_lwbrx_lfs, probe_byterev_lfdx, probe_byterev_ldbrx_lfd, probe_byterev_chase_lfdx,
	probe_byterev_chase_ldbrx_lfd, probe_byterev_stfsx, probe_byterev_stfs_stwbrx, probe_byterev_stfdx,
	probe_byterev_stfd_stdbrx, probe_byterev_lvx, probe_byterev_lvx_words, probe_byterev_lvx_whole, probe_byterev_stvx,
	probe_byterev_words_stvx, probe_byterev_whole_stvx, probe_byterev_add_chain, probe_byterev_add_stream;

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
	{ "float load: lfsx", probe_byterev_lfsx, FLOAT_LOAD, 4, 0 },
	{ "float load: lwbrx; stw; lfs", probe_byterev_lwbrx_lfs, FLOAT_LOAD, 4, LOAD_REVERSED },
	{ "float chase: lfsx; stfs; lwz; clrlwi", probe_byterev_chase_lfsx, FLOAT_CHASE, 4, 0 },
	{ "float chase: lwbrx; stw; lfs; stfs; lwz; clrlwi", probe_byterev_chase_lwbrx_lfs, FLOAT_CHASE, 4, LOAD_REVERSED },
	{ "double load: lfdx", probe_byterev_lfdx, FLOAT_LOAD, 8, 0 },
	{ "double chase: lfdx; stfd; ld; clrlwi", probe_byterev_chase_lfdx, FLOAT_CHASE, 8, 0 },
	{ "float store: stfsx", probe_byterev_stfsx, FLOAT_STORE, 4, 0 },
	{ "float store: stfs; lwz; stwbrx", probe_byterev_stfs_stwbrx, FLOAT_STORE, 4, STORE_REVERSED },
	{ "double store: stfdx", probe_byterev_stfdx, FLOAT_STORE, 8, 0 },
	{ "vector load: lvx", probe_byterev_lvx, VECTOR_LOAD, 1, 0 },
	{ "vector load: lvx; vperm each word", probe_byterev_lvx_words, VECTOR_LOAD, 4, 0 },
	{ "vector load: lvx; vperm the whole", probe_byterev_lvx_whole, VECTOR_LOAD, 16, 0 },
	{ "vector store: stvx", probe_byterev_stvx, VECTOR_STORE, 1, 0 },
	{ "vector store: vperm each word; stvx", probe_byterev_words_stvx, VECTOR_STORE, 4, 0 },
	{ "vector store: vperm the whole; stvx", probe_byterev_whole_stvx, VECTOR_STORE, 16, 0 },
	{ "add: one chain", probe_byterev_add_chain, ADD_CHAIN, 0, 0 },
	{ "add: eight independent", probe_byterev_add_stream, ADD_STREAM, 0, 0 },
};

// ldbrx and stdbrx, which the console may refuse, so they run after the rest.
static const Test last_tests[] = {
	{ "load: ldbrx", probe_byterev_ldbrx, LOAD, 8, LOAD_REVERSED },
	{ "chase: ldbrx", probe_byterev_chase_ldbrx, CHASE, 8, LOAD_REVERSED },
	{ "store: stdbrx", probe_byterev_stdbrx, STORE, 8, STORE_REVERSED },
	{ "double load: ldbrx; std; lfd", probe_byterev_ldbrx_lfd, FLOAT_LOAD, 8, LOAD_REVERSED },
	{ "double chase: ldbrx; std; lfd; stfd; ld; clrlwi", probe_byterev_chase_ldbrx_lfd, FLOAT_CHASE, 8, LOAD_REVERSED },
	{ "double store: stfd; ld; stdbrx", probe_byterev_stfd_stdbrx, FLOAT_STORE, 8, STORE_REVERSED },
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

// The pattern byte at index i of a vector after vperm reverses each group of group_bytes, 1 for no reversal.
static uint8_t reversed_byte(uint32_t slot, uint32_t i, uint32_t group_bytes)
{
	return pattern_byte(slot, i / group_bytes * group_bytes + group_bytes - 1 - i % group_bytes);
}

// An FPR's 64 bits after an lfs or lfd of the 32 or 64 bits.
static uint64_t fpr_bits(uint64_t bits, uint32_t size_bytes)
{
	if (size_bytes == 8)
		return bits;
	const uint32_t single_bits = (uint32_t)bits;
	float single;
	memcpy(&single, &single_bits, sizeof single);
	const double widened = single;
	uint64_t double_bits;
	memcpy(&double_bits, &widened, sizeof double_bits);
	return double_bits;
}

// Each entry holds the offset of the one 17 on.
static void fill_chain(const Test *test)
{
	const uint32_t entries = BUFFER_BYTES / test->size_bytes;
	// A float chase's entries are floats from 1.0 up with the offset in their low bits, not denormals.
	const uint64_t one = test->size_bytes == 4 ? 0x3F800000 : 0x3FF0000000000000;
	for (uint32_t i = 0; i < entries; i++) {
		const uint64_t next =
			(uint64_t)((i + CHASE_STRIDE) % entries) * test->size_bytes | (test->kind == FLOAT_CHASE ? one : 0);
		store_image(buffer + i * test->size_bytes, next, test->size_bytes, test->reversed & LOAD_REVERSED);
	}
}

static void prepare(const Test *test)
{
	memset(buffer, 0, sizeof buffer);
	memset(values, 0, sizeof values);
	switch (test->kind) {
	case LOAD:
	case FLOAT_LOAD:
	case VECTOR_LOAD:
		for (uint32_t slot = 0; slot < SLOTS; slot++) {
			for (uint32_t i = 0; i < 16; i++)
				buffer[slot * SLOT_STRIDE_BYTES + i] = pattern_byte(slot, i);
		}
		break;
	case CHASE:
	case FLOAT_CHASE:
		fill_chain(test);
		break;
	case STORE:
		for (uint32_t slot = 0; slot < SLOTS; slot++)
			values[slot] = slot_value(slot, 8, false);
		break;
	case FLOAT_STORE:
		for (uint32_t slot = 0; slot < SLOTS; slot++)
			values[slot] = fpr_bits(slot_value(slot, test->size_bytes, false), test->size_bytes);
		break;
	case STORE_THEN_LOAD:
		values[0] = slot_value(0, 4, false);
		break;
	case VECTOR_STORE:
		for (uint32_t slot = 0; slot < SLOTS; slot++) {
			for (uint32_t i = 0; i < 16; i++)
				((uint8_t *)values)[16 * slot + i] = pattern_byte(slot, i);
		}
		break;
	case ADD_CHAIN:
	case ADD_STREAM:
		for (uint32_t slot = 0; slot < SLOTS; slot++)
			values[slot] = ADDEND + slot;
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
	case FLOAT_LOAD:
		for (uint32_t slot = 0; slot < SLOTS; slot++)
			ok = ok && values[slot] == fpr_bits(slot_value(slot, size_bytes, load_reversed), size_bytes);
		break;
	case CHASE:
	case FLOAT_CHASE: {
		const uint64_t entries = BUFFER_BYTES / size_bytes;
		const uint64_t steps = (uint64_t)passes * OPERATIONS_PER_PASS;
		ok = values[0] == steps * CHASE_STRIDE % entries * size_bytes;
		break;
	}
	case STORE:
	case FLOAT_STORE:
		for (uint32_t slot = 0; slot < SLOTS; slot++) {
			uint8_t expected[8] = { 0 };
			// A float is stored as the bits it was made from, not as the 64 bits of the register.
			const uint64_t stored = test->kind == FLOAT_STORE ? slot_value(slot, size_bytes, false) : values[slot];
			store_image(expected, stored, size_bytes, store_reversed);
			ok = ok && memcmp(buffer + slot * SLOT_STRIDE_BYTES, expected, sizeof expected) == 0;
		}
		break;
	case VECTOR_LOAD:
	case VECTOR_STORE: {
		// The loaded vectors are in values, 16 bytes apart; the stored ones in the buffer, a slot apart.
		const bool loaded = test->kind == VECTOR_LOAD;
		const uint8_t *const result = loaded ? (const uint8_t *)values : buffer;
		for (uint32_t slot = 0; slot < SLOTS; slot++) {
			for (uint32_t i = 0; i < 16; i++)
				ok = ok && result[slot * (loaded ? 16 : SLOT_STRIDE_BYTES) + i] == reversed_byte(slot, i, size_bytes);
		}
		break;
	}
	case ADD_CHAIN:
		ok = values[0] == (uint64_t)passes * OPERATIONS_PER_PASS * ADDEND;
		break;
	case ADD_STREAM:
		for (uint32_t slot = 0; slot < SLOTS; slot++)
			ok = ok && values[slot] == (uint64_t)passes * (ADDEND + slot);
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

// Five seconds, relative, in 100 ns units.
static const int64_t WAIT_TIMEOUT = -50000000;

// Blocks of SPIN_PASSES passes the spinner has counted, and whether it is to stop.
static volatile uint32_t spinner_blocks;
static volatile uint32_t spinner_stop;

// Adds on the sibling hardware thread as fast as it can; the adds take as long whatever they add, so the addends are 0.
static uint32_t spin(void *context)
{
	(void)context;
	uint64_t addends[VALUE_COUNT] = { 0 };
	xsdk_log_write("byterev: the spinner runs on hardware thread %u", (unsigned)xsdk_thread_hardware_thread());
	while (!spinner_stop) {
		probe_byterev_add_stream(NULL, SPIN_PASSES, addends);
		spinner_blocks = spinner_blocks + 1;
	}
	return 0;
}

// With the spinner running, runs the test ROUNDS times, or sleeps for NULL, and logs how fast the spinner counted.
static bool measure_beside_spinner(const Test *test)
{
	const char *const what = test ? test->name : "idle";
	const uint32_t passes = test ? passes_for_target(test) : 0;
	double rates[ROUNDS];
	bool ok = true;
	for (uint32_t round = 0; round < ROUNDS; round++) {
		const uint32_t blocks = spinner_blocks;
		const uint32_t start = probe_time_base();
		uint32_t ticks;
		if (test)
			ok = run_once(test, passes, &ticks) && ok;
		else
			(void)xsdk_thread_sleep(IDLE_MS);
		const uint32_t window_ticks = probe_time_base() - start;
		rates[round] = (double)(spinner_blocks - blocks) * SPIN_PASSES * TIME_BASE_HZ / window_ticks / 1e6;
	}
	sort_doubles(rates, ROUNDS);
	ok = ok && rates[0] > 0;
	xsdk_log_write(
		"%s: byterev sibling, main thread %s: the spinner counted %.2f million passes of 8 adds a second at "
		"worst, %.2f the median, %.2f the best",
		ok ? "PASS" : "FAIL", what, rates[0], rates[ROUNDS / 2], rates[ROUNDS - 1]);
	return ok;
}

static bool measure_beside_spinner_running(Kernel *run)
{
	for (uint32_t i = 0; i < TESTS; i++) {
		if (tests[i].run == run)
			return measure_beside_spinner(&tests[i]);
	}
	xsdk_log_write("FAIL: byterev sibling: the kernel isn't a test");
	return false;
}

// Measures how fast the spinner counts on the main thread's sibling while the main thread is idle, adds, and loads with
// lwzx and with lwbrx; returns the failures.
static uint32_t measure_with_sibling(void)
{
	HANDLE spinner;
	const NTSTATUS status = xsdk_thread_create_on(&spinner, SPINNER_STACK_BYTES, spin, NULL, SIBLING_HARDWARE_THREAD);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("FAIL: byterev sibling: xsdk_thread_create_on(%u) failed: 0x%08X",
			(unsigned)SIBLING_HARDWARE_THREAD, (unsigned)status);
		return 1;
	}
	for (uint32_t ms = 0; !spinner_blocks && ms < SPINNER_START_MS; ms++)
		(void)xsdk_thread_sleep(1);
	uint32_t failed = 0;
	if (spinner_blocks) {
		failed += !measure_beside_spinner(NULL);
		failed += !measure_beside_spinner_running(probe_byterev_add_stream);
		failed += !measure_beside_spinner_running(probe_byterev_lwzx);
		failed += !measure_beside_spinner_running(probe_byterev_lwbrx);
	} else {
		xsdk_log_write("FAIL: byterev sibling: the spinner counted nothing in %u ms", (unsigned)SPINNER_START_MS);
		failed++;
	}
	spinner_stop = 1;
	const NTSTATUS waited = NtWaitForSingleObjectEx(spinner, USER_MODE, 0, &WAIT_TIMEOUT);
	(void)NtClose(spinner);
	if (waited != STATUS_SUCCESS) {
		xsdk_log_write("FAIL: byterev sibling: waiting for the spinner returned 0x%08X", (unsigned)waited);
		failed++;
	}
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
	failed += measure_with_sibling();
	failed += measure_all(last_tests, LAST_TESTS);
	xsdk_log_write("TESTS COMPLETE: status=%d", failed ? 1 : 0);
	return failed ? 1 : 0;
}
