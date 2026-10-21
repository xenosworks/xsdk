#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "probe.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

// Milestone 55 step 2; modes and console evidence: docs/scope/55-windows-threads-and-memory.md.

enum {
	// elf2xex marks execinstr pages Code (tools/elf2xex.py, page_descriptors).
	JIT_BYTES = 0x10000,
	// The Xenon's cache line (libxenon:drivers/ppc/cache.S:1).
	CACHE_LINE_BYTES = 128,
	// li r3, N is addi r3, 0, N (opcode 14): 0x38600000 | N. blr is 0x4E800020.
	LI_R3 = 0x38600000,
	BLR = 0x4E800020,
	T1_VALUE = 42,
	T2_VALUE = 99,
	MODE_BYTES = 32,
};

__asm__(
	".section .jit,\"awx\",@nobits\n"
	".balign 0x10000\n"
	".globl probe_jit_pool\n"
	"probe_jit_pool:\n"
	".skip 0x10000\n"
	".previous\n");
extern uint32_t probe_jit_pool[];

typedef uint32_t (*JitFunction)(void);

static uint32_t passed_count;
static uint32_t failed_count;

static void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	passed_count += passed;
	failed_count += !passed;
}

// PowerPC Book II v2.01, section 1.8, p. 11 (link in docs/scope/55-windows-threads-and-memory.md).
static void code_flush(const void *start, uint32_t bytes)
{
	const uintptr_t first = (uintptr_t)start & ~(uintptr_t)(CACHE_LINE_BYTES - 1);
	const uintptr_t end = (uintptr_t)start + bytes;
	for (uintptr_t line = first; line < end; line += CACHE_LINE_BYTES)
		__asm__ volatile("dcbst 0, %0" : : "r"(line) : "memory");
	__asm__ volatile("sync" : : : "memory");
	for (uintptr_t line = first; line < end; line += CACHE_LINE_BYTES)
		__asm__ volatile("icbi 0, %0" : : "r"(line) : "memory");
	__asm__ volatile("sync" : : : "memory");
	__asm__ volatile("isync" : : : "memory");
}

// Writes "li r3, value; blr" at the pool's start, as plain stores into cached memory.
static void write_return(uint32_t value)
{
	probe_jit_pool[0] = LI_R3 | (value & 0xFFFF);
	probe_jit_pool[1] = BLR;
}

static bool resolve(uint32_t ordinal, void **slot)
{
	HANDLE kernel = 0;
	if (XexGetModuleHandle("xboxkrnl.exe", &kernel) != 0)
		return false;
	return XexGetProcedureAddress(kernel, ordinal, slot) >= 0 && *slot;
}

static void query_case(void)
{
	xsdk_log_write("exec: query: the .jit section's protection; nothing runs");
	const uint32_t base = (uint32_t)(uintptr_t)probe_jit_pool;
	xsdk_log_write("exec: .jit pool at %08X, %u bytes", (unsigned)base, (unsigned)JIT_BYTES);

	typedef uint32_t (*QueryProtect)(uint32_t);
	typedef NTSTATUS (*QueryVirtual)(uint32_t, MEMORY_BASIC_INFORMATION *);
	QueryProtect query_protect;
	QueryVirtual query_virtual;
	if (resolve(0x0C4, (void **)&query_protect))
		xsdk_log_write("exec: MmQueryAddressProtect(.jit) = %08X", (unsigned)query_protect(base));
	if (resolve(0x0EE, (void **)&query_virtual)) {
		MEMORY_BASIC_INFORMATION info = { 0 };
		const NTSTATUS status = query_virtual(base, &info);
		xsdk_log_write("exec: NtQueryVirtualMemory(.jit) = %08X: state %08X, protect %08X, size %08X", (unsigned)status,
			(unsigned)info.state, (unsigned)info.protect, (unsigned)info.region_size);
	}
}

static void spike_case(void)
{
	xsdk_log_write("exec: spike: write code into the .jit section, flush, call; may hang if .jit isn't executable");
	const uint32_t base = (uint32_t)(uintptr_t)probe_jit_pool;
	const JitFunction fn = (JitFunction)(void *)probe_jit_pool;

	xsdk_log_write("exec: T1: writing li r3, %u; blr at %08X", (unsigned)T1_VALUE, (unsigned)base);
	write_return(T1_VALUE);
	code_flush(probe_jit_pool, 2 * sizeof(uint32_t));
	xsdk_log_write("exec: T1: calling the .jit code");
	const uint32_t t1 = fn();
	xsdk_log_write("exec: T1: returned %u, want %u", (unsigned)t1, (unsigned)T1_VALUE);
	check("exec: a function in the .jit section runs and returns 42", t1 == T1_VALUE);

	xsdk_log_write("exec: T2: rewriting li r3, %u; blr", (unsigned)T2_VALUE);
	write_return(T2_VALUE);
	code_flush(probe_jit_pool, 2 * sizeof(uint32_t));
	xsdk_log_write("exec: T2: calling the rewritten .jit code");
	const uint32_t t2 = fn();
	xsdk_log_write("exec: T2: returned %u, want %u", (unsigned)t2, (unsigned)T2_VALUE);
	check("exec: the rewritten function runs and returns 99", t2 == T2_VALUE);

	// Logging above may have evicted the old code; this observation cannot establish whether a flush is needed.
	xsdk_log_write("exec: rewriting li r3, %u; blr WITHOUT a flush", (unsigned)T1_VALUE);
	write_return(T1_VALUE);
	const uint32_t stale = fn();
	xsdk_log_write("exec: without a flush the call returned %u (42 means the write was seen, 99 means it wasn't)",
		(unsigned)stale);
	code_flush(probe_jit_pool, 2 * sizeof(uint32_t));
}

static void read_mode(char *mode)
{
	strcpy(mode, "spike");
	FILE *file = fopen("probe_exec.txt", "r");
	if (!file)
		return;
	char line[MODE_BYTES] = "";
	if (fgets(line, sizeof line, file))
		line[strcspn(line, "\r\n ")] = '\0';
	fclose(file);
	if (line[0])
		strcpy(mode, line);
}

int main(void)
{
	char mode[MODE_BYTES];
	read_mode(mode);
	xsdk_log_write("probe_exec: mode %s; .jit pool at %08X, %u bytes", mode, (unsigned)(uintptr_t)probe_jit_pool,
		(unsigned)JIT_BYTES);
	if (strcmp(mode, "query") == 0)
		query_case();
	else
		spike_case();
	xsdk_log_write("probe_exec: %u passed, %u failed", (unsigned)passed_count, (unsigned)failed_count);
	return 0;
}
