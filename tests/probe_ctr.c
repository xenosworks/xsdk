#include <stdbool.h>
#include <stdint.h>

#include "xsdk/log.h"

// Milestone 52 step 1's probe (docs/scope/52-compiled-cpu-code.md): whether bdnz tests all of CTR in a title, as it
// does with MSR[SF] set, or only its low word. Xenia tests the low word
// (xenia:src/xenia/cpu/ppc/ppc_emit_control.cc:193, 299). Two more signs of the mode follow, the carry and MSR itself.

enum {
	// A count through CTR's high word would take 2^32 passes; the loop stops here.
	PASS_LIMIT = 1000,
	LOW_WORD_PASSES = 3,
	XER_CA_SHIFT = 29,
};

static const uint64_t patterns[] = { 0x0000000000000003ull, 0x0000000100000003ull, 0xFFFFFFFF00000003ull };

enum { PATTERNS = sizeof patterns / sizeof patterns[0] };

static const uint64_t MSR_SF = 1ull << 63;
static const uint64_t MSR_HV = 1ull << 60;
static const uint64_t MSR_PR = 1ull << 14;

// Counts bdnz's passes from ctr, PASS_LIMIT at most; *after gets CTR as the loop left it.
static uint32_t count_passes(uint64_t ctr, uint64_t *after)
{
	uint32_t passes = 0;
	uint64_t left;
	__asm__ volatile(
		"mtctr %2\n"
		"1:\taddi %0,%0,1\n"
		"\tcmplwi %0,%3\n"
		"\tbeq 2f\n"
		"\tbdnz 1b\n"
		"2:\tmfctr %1"
		: "+b"(passes), "=r"(left)
		: "r"(ctr), "i"(PASS_LIMIT)
		: "ctr", "cr0");
	*after = left;
	return passes;
}

// addic takes its carry from bit 32 in 32-bit mode and from the top bit in 64-bit mode.
static uint32_t carry_out_of_low_word(void)
{
	uint64_t value = 0xFFFFFFFFull;
	uint32_t xer;
	__asm__ volatile(
		"addic %0,%0,1\n"
		"\tmfxer %1"
		: "+r"(value), "=r"(xer)
		:
		: "ca");
	return xer >> XER_CA_SHIFT & 1;
}

static const char *verdict(bool baseline, bool low_word, bool all_bits)
{
	if (!baseline)
		return "the probe is broken: 3 didn't count 3";
	if (low_word)
		return "bdnz tests CTR's low word only, as in 32-bit mode";
	if (all_bits)
		return "bdnz tests all 64 bits of CTR, as in 64-bit mode";
	return "bdnz did neither";
}

int main(void)
{
	bool baseline = false;
	bool low_word = true;
	bool all_bits = true;
	for (uint32_t i = 0; i < PATTERNS; i++) {
		uint64_t after;
		const uint32_t passes = count_passes(patterns[i], &after);
		xsdk_log_write("ctr probe: CTR %016llX: bdnz passed %u times%s, CTR left %016llX",
			(unsigned long long)patterns[i], (unsigned)passes, passes == PASS_LIMIT ? " (stopped at the limit)" : "",
			(unsigned long long)after);
		if (i == 0) {
			baseline = passes == LOW_WORD_PASSES;
			continue;
		}
		low_word = low_word && passes == LOW_WORD_PASSES;
		all_bits = all_bits && passes == PASS_LIMIT;
	}
	xsdk_log_write("ctr probe: %s", verdict(baseline, low_word, all_bits));

	const uint32_t carry = carry_out_of_low_word();
	xsdk_log_write("ctr probe: 0xFFFFFFFF + 1 carried %u, as in %s mode", (unsigned)carry, carry ? "32-bit" : "64-bit");

	// Last, as a fault ends the title and goes back to the dashboard (docs/HARDWARE.md, milestone 40). The context a
	// fault handler gets suggests MSR 0xB030, PR clear, so mfmsr shouldn't fault.
	xsdk_log_write("ctr probe: reading MSR; nothing after this line means mfmsr faulted");
	uint64_t msr;
	__asm__ volatile("mfmsr %0" : "=r"(msr));
	xsdk_log_write("ctr probe: MSR %016llX: SF %u, HV %u, PR %u", (unsigned long long)msr, (unsigned)!!(msr & MSR_SF),
		(unsigned)!!(msr & MSR_HV), (unsigned)!!(msr & MSR_PR));
	return 0;
}
