#include <stdint.h>

#include "xsdk/export.h"
#include "xsdk/kernel.h"

// The DLL dll_check loads (tests/dll_check.c): functions it exports by ordinal, read-only and writable data, a kernel
// import of its own, and a count of each reason its DllMain was called with.

enum {
	DLL_PROCESS_DETACH = 0,
	REASONS = 4,
};

static const char NAME[] = "dll_target";
static uint32_t calls[REASONS];
static uint32_t counter = 100;
// Where DllMain writes the number of DLL_PROCESS_DETACH calls, which come after the title can no longer ask.
static uint32_t *detached;

uint32_t DllMain(HANDLE module, uint32_t reason, void *reserved)
{
	(void)module;
	(void)reserved;
	if (reason < REASONS)
		calls[reason]++;
	if (reason == DLL_PROCESS_DETACH && detached)
		*detached = calls[DLL_PROCESS_DETACH];
	return 1;
}

uint32_t dll_target_add(uint32_t a, uint32_t b)
{
	return a + b;
}

uint32_t dll_target_calls(uint32_t reason)
{
	return reason < REASONS ? calls[reason] : 0;
}

const char *dll_target_name(void)
{
	return NAME;
}

uint32_t dll_target_count(void)
{
	return ++counter;
}

// Through the DLL's own import thunk, which the loader binds as it loads the DLL.
uint64_t dll_target_frequency(void)
{
	return KeQueryPerformanceFrequency();
}

void dll_target_watch(uint32_t *word)
{
	detached = word;
}

XSDK_EXPORT(1, dll_target_add);
XSDK_EXPORT(2, dll_target_calls);
XSDK_EXPORT(3, dll_target_name);
XSDK_EXPORT(4, dll_target_count);
XSDK_EXPORT(5, dll_target_frequency);
XSDK_EXPORT(6, dll_target_watch);
