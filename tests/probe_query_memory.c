#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xsdk/jit.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"

enum { PAGE_BYTES = 4096, RESERVE_BYTES = 65536, GUARD_WORDS = 32, SPLASH_MS = 2000 };
// Xenia's state values (xenia:src/xenia/xbox.h:130).
enum { EXPECT_FREE = 0x10000 };

static unsigned passed_count;
static unsigned failed_count;

static void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	passed_count += passed;
	failed_count += !passed;
}

static bool run_query(const char *name, uint32_t address, uint32_t state, uint32_t allocation, NTSTATUS expected_status)
{
	_Alignas(16) struct {
		volatile uint32_t before[GUARD_WORDS];
		MEMORY_BASIC_INFORMATION info;
		volatile uint32_t after[GUARD_WORDS];
	} report;
	memset(&report, 0xA5, sizeof(report));
	const uint32_t protection = state == MEM_COMMIT ? MmQueryAddressProtect(address) : 0;
	if (state != EXPECT_FREE)
		xsdk_log_write("BEGIN query: %s address=%08X output=%08X third=00000000 expected state=%08X protect=%08X", name,
			(unsigned)address, (unsigned)(uintptr_t)&report.info, (unsigned)state, (unsigned)protection);
	const NTSTATUS status = NtQueryVirtualMemory(address, &report.info, 0);
	xsdk_log_write("END query: %s status=0x%08X expected=0x%08X", name, (unsigned)status, (unsigned)expected_status);
	bool guards = true;
	for (unsigned i = 0; i < GUARD_WORDS; i++)
		guards &= report.before[i] == 0xA5A5A5A5 && report.after[i] == 0xA5A5A5A5;
	check("query: output guards intact", guards);
	if (!guards) {
		xsdk_log_write("STOP: unexpected output size; no more query calls");
		exit(1);
	}
	if (status != STATUS_SUCCESS || expected_status != STATUS_SUCCESS) {
		check(name, status == expected_status);
		return status == expected_status;
	}
	const MEMORY_BASIC_INFORMATION *const info = &report.info;
	xsdk_log_write(
		"query: base=%08X allocation=%08X allocation_protect=%08X size=%08X state=%08X protect=%08X type=%08X",
		(unsigned)info->base_address, (unsigned)info->allocation_base, (unsigned)info->allocation_protect,
		(unsigned)info->region_size, (unsigned)info->state, (unsigned)info->protect, (unsigned)info->type);
	const bool contains = info->region_size && info->base_address <= address &&
		(uint64_t)address < (uint64_t)info->base_address + info->region_size &&
		(uint64_t)info->base_address + info->region_size <= (UINT64_C(1) << 32);
	const bool matches = status == STATUS_SUCCESS && contains && info->state == state &&
		(!allocation || info->allocation_base == allocation) &&
		(state != MEM_COMMIT || (info->protect & 0xFF) == (protection & 0xFF));
	check(name, matches);
	return matches;
}

static void allocation_queries(void)
{
	void *base = NULL;
	uint32_t bytes = RESERVE_BYTES;
	NTSTATUS status = NtAllocateVirtualMemory(&base, &bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, 0);
	xsdk_log_write("query: allocate status=%08X base=%08X bytes=%08X", (unsigned)status, (unsigned)(uintptr_t)base,
		(unsigned)bytes);
	check("query: allocate controlled pages", NT_SUCCESS(status) && base && bytes >= 2 * PAGE_BYTES);
	if (!NT_SUCCESS(status) || !base)
		return;
	const uint32_t address = (uint32_t)(uintptr_t)base;
	(void)run_query("query: committed read-write page", address, MEM_COMMIT, address, STATUS_SUCCESS);
	(void)run_query("query: interior address", address + 17, MEM_COMMIT, address, STATUS_SUCCESS);
	void *changed = base;
	uint32_t length = PAGE_BYTES;
	uint32_t old_protection = 0;
	status = NtProtectVirtualMemory(&changed, &length, PAGE_READONLY, &old_protection, 0);
	xsdk_log_write("query: protect status=%08X old=%08X", (unsigned)status, (unsigned)old_protection);
	check("query: protect first page read-only without changing its neighbour",
		NT_SUCCESS(status) && (MmQueryAddressProtect(address) & 0xFF) == PAGE_READONLY &&
			(MmQueryAddressProtect(address + PAGE_BYTES) & 0xFF) == PAGE_READWRITE);
	if (NT_SUCCESS(status)) {
		(void)run_query("query: read-only page", address, MEM_COMMIT, address, STATUS_SUCCESS);
		(void)run_query(
			"query: neighbouring read-write page", address + PAGE_BYTES, MEM_COMMIT, address, STATUS_SUCCESS);
	}
	changed = base;
	length = PAGE_BYTES;
	status = NtFreeVirtualMemory(&changed, &length, MEM_DECOMMIT, 0);
	xsdk_log_write("query: decommit status=%08X", (unsigned)status);
	check("query: decommit first page", NT_SUCCESS(status));
	if (NT_SUCCESS(status))
		(void)run_query("query: reserved page after decommit", address, MEM_RESERVE, address, STATUS_SUCCESS);
	changed = base;
	length = 0;
	xsdk_log_write("BEGIN release and free query: address=%08X third=00000000", (unsigned)address);
	status = NtFreeVirtualMemory(&changed, &length, MEM_RELEASE, 0);
	// Query immediately, before logging can allocate in the released range.
	if (NT_SUCCESS(status))
		(void)run_query("query: free range after release", address, EXPECT_FREE, 0, STATUS_SUCCESS);
	xsdk_log_write("query: release status=%08X", (unsigned)status);
	check("query: release allocation", NT_SUCCESS(status));
}

int main(void)
{
	_Static_assert(sizeof(MEMORY_BASIC_INFORMATION) == 28, "seven-word query layout");
	xsdk_log_write("probe_query_memory v2: NtQueryVirtualMemory(address, information, 0)");
	xsdk_log_write(
		"Allocated regions are queryable; image pages require MmQueryAddressProtect; only third=0 is tested");
	const NTSTATUS splash = xsdk_splash_show(SPLASH_MS);
	xsdk_log_write("query: splash status=%08X", (unsigned)splash);
	check("splash: the vulkan4xenon logo shown", NT_SUCCESS(splash));
	allocation_queries();
	(void)run_query("query: title code refused", (uint32_t)(uintptr_t)&main, 0, 0, STATUS_INVALID_PARAMETER);
	void *pool;
	size_t bytes;
	xsdk_jit_pool(&pool, &bytes);
	check("query: executable image pool present", bytes != 0);
	if (bytes)
		(void)run_query("query: jit image pool refused", (uint32_t)(uintptr_t)pool, 0, 0, STATUS_INVALID_PARAMETER);
	xsdk_log_write("query: %u passed, %u failed", passed_count, failed_count);
	return failed_count != 0;
}
