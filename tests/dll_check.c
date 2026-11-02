#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"

// Loads dll_target.xex beside it, a DLL built by elf2xex --dll from tests/dll_target.c, and checks what a title gets:
// its exports by ordinal, its data, its own kernel import, and the calls to its DllMain as it loads, as a thread starts
// and ends, and as the last reference to it goes.

enum {
	DLL_PROCESS_DETACH = 0,
	DLL_PROCESS_ATTACH = 1,
	DLL_THREAD_ATTACH = 2,
	DLL_THREAD_DETACH = 3,
	ORDINAL_ADD = 1,
	ORDINAL_CALLS = 2,
	ORDINAL_NAME = 3,
	ORDINAL_COUNT = 4,
	ORDINAL_FREQUENCY = 5,
	ORDINAL_WATCH = 6,
	EXPORTS = 6,
	// Where the Makefile links it: its --base, and the 64 KB of its first page and more the test DLL fills.
	DLL_BASE = 0x8E000000,
	DLL_BYTES = 0x40000,
};

static const char DLL_PATH[] = "game:\\dll_target.xex";
static const char DLL_NAME[] = "dll_target.xex";
static const char MISSING_PATH[] = "game:\\dll_none.xex";

typedef uint32_t (*AddFunction)(uint32_t a, uint32_t b);
typedef uint32_t (*CallsFunction)(uint32_t reason);
typedef const char *(*NameFunction)(void);
typedef uint32_t (*CountFunction)(void);
typedef uint64_t (*FrequencyFunction)(void);
typedef void (*WatchFunction)(uint32_t *word);

static int passed;
static int failed;

static void check(const char *name, bool ok)
{
	xsdk_log_write("%s: dll: %s", ok ? "PASS" : "FAIL", name);
	ok ? passed++ : failed++;
}

static void *thread_body(void *argument)
{
	return argument;
}

int main(void)
{
	HANDLE module = 0;
	const NTSTATUS loaded = XexLoadImage(DLL_PATH, XEX_LOAD_IMAGE_DLL, 0, &module);
	xsdk_log_write("dll: XexLoadImage(%s): 0x%08X, handle 0x%08X", DLL_PATH, (unsigned)loaded, (unsigned)module);
	check("XexLoadImage loads the DLL beside the title", NT_SUCCESS(loaded) && module);
	if (!NT_SUCCESS(loaded) || !module) {
		xsdk_log_write("dll_check: %d passed, %d failed", passed, failed);
		return 1;
	}

	void *exports[EXPORTS + 1] = { 0 };
	bool found = true;
	for (uint32_t ordinal = 1; ordinal <= EXPORTS; ordinal++) {
		const NTSTATUS status = XexGetProcedureAddress(module, ordinal, &exports[ordinal]);
		const uintptr_t address = (uintptr_t)exports[ordinal];
		xsdk_log_write("dll: ordinal %u: 0x%08X at 0x%08X", (unsigned)ordinal, (unsigned)status, (unsigned)address);
		found = found && NT_SUCCESS(status) && address >= DLL_BASE && address < DLL_BASE + DLL_BYTES;
	}
	check("each export's ordinal gives an address in the DLL, at its --base", found);
	if (!found) {
		xsdk_log_write("dll_check: %d passed, %d failed", passed, failed);
		return 1;
	}
	const AddFunction add = (AddFunction)exports[ORDINAL_ADD];
	const CallsFunction calls = (CallsFunction)exports[ORDINAL_CALLS];
	const NameFunction name = (NameFunction)exports[ORDINAL_NAME];
	const CountFunction count = (CountFunction)exports[ORDINAL_COUNT];
	const FrequencyFunction frequency = (FrequencyFunction)exports[ORDINAL_FREQUENCY];
	const WatchFunction watch = (WatchFunction)exports[ORDINAL_WATCH];

	check("an exported function runs: 2 + 3 is 5", add(2, 3) == 5);
	check("the DLL's read-only data is there", strcmp(name(), "dll_target") == 0);
	const uint32_t first = count();
	check("the DLL's data starts as linked and can be written", first == 101 && count() == 102);
	const uint64_t dll_frequency = frequency();
	xsdk_log_write("dll: KeQueryPerformanceFrequency through the DLL: %llu", (unsigned long long)dll_frequency);
	check("the DLL's own kernel import is bound", dll_frequency && dll_frequency == KeQueryPerformanceFrequency());
	check("DllMain ran once with DLL_PROCESS_ATTACH as the DLL loaded", calls(DLL_PROCESS_ATTACH) == 1);

	const uint32_t attached = calls(DLL_THREAD_ATTACH);
	const uint32_t detached = calls(DLL_THREAD_DETACH);
	pthread_t thread;
	const bool joined = pthread_create(&thread, NULL, thread_body, NULL) == 0 && pthread_join(thread, NULL) == 0;
	xsdk_log_write("dll: a thread: DLL_THREAD_ATTACH %u to %u, DLL_THREAD_DETACH %u to %u", (unsigned)attached,
		(unsigned)calls(DLL_THREAD_ATTACH), (unsigned)detached, (unsigned)calls(DLL_THREAD_DETACH));
	check("a new thread calls DllMain with DLL_THREAD_ATTACH and DLL_THREAD_DETACH",
		joined && calls(DLL_THREAD_ATTACH) > attached && calls(DLL_THREAD_DETACH) > detached);

	HANDLE by_name = 0;
	const uint32_t named = XexGetModuleHandle(DLL_NAME, &by_name);
	check("XexGetModuleHandle finds the DLL by name", named == 0 && by_name == module);

	HANDLE again = 0;
	const NTSTATUS reloaded = XexLoadImage(DLL_PATH, XEX_LOAD_IMAGE_DLL, 0, &again);
	check("loading it again gives the same module without running DllMain again",
		NT_SUCCESS(reloaded) && again == module && calls(DLL_PROCESS_ATTACH) == 1);

	uint32_t process_detaches = 0;
	watch(&process_detaches);
	const NTSTATUS first_unload = XexUnloadImage(module);
	HANDLE still = 0;
	const bool kept = XexGetModuleHandle(DLL_NAME, &still) == 0 && still == module;
	check("the first of two unloads keeps the DLL loaded", NT_SUCCESS(first_unload) && kept && !process_detaches);
	const NTSTATUS last_unload = XexUnloadImage(module);
	HANDLE gone = 0;
	const bool unloaded = XexGetModuleHandle(DLL_NAME, &gone) != 0;
	xsdk_log_write("dll: unloads 0x%08X and 0x%08X, DLL_PROCESS_DETACH %u", (unsigned)first_unload,
		(unsigned)last_unload, (unsigned)process_detaches);
	check("the last unload calls DllMain with DLL_PROCESS_DETACH and unloads the DLL",
		NT_SUCCESS(last_unload) && process_detaches == 1 && unloaded);

	HANDLE missing = 0;
	const NTSTATUS not_there = XexLoadImage(MISSING_PATH, XEX_LOAD_IMAGE_DLL, 0, &missing);
	xsdk_log_write("dll: XexLoadImage(%s): 0x%08X", MISSING_PATH, (unsigned)not_there);
	check("loading a DLL that isn't there fails", !NT_SUCCESS(not_there) && !missing);

	xsdk_log_write("dll_check: %d passed, %d failed", passed, failed);
	return failed ? 1 : 0;
}
