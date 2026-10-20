#include <stdint.h>

// A counted loop that GCC must end in bdnz; the Makefile checks it with the configured toolchain (milestone 52).
uint32_t doloop_check(const uint32_t *words, uint32_t count);

uint32_t doloop_check(const uint32_t *words, uint32_t count)
{
	uint32_t sum = 0;
	for (uint32_t i = 0; i < count; i++)
		sum += words[i] * 3;
	return sum;
}
