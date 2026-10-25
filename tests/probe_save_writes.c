#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "probe.h"
#include "xsdk/log.h"

// Milestone 61's probe: what a save written as libultraship writes Starship's costs on the console's disk. The game
// saves 8 bytes at a time, and for each, libultraship reads the 512-byte file whole, then makes it again with "wb" and
// writes all 512 bytes (lus:src/public/libultra/os_eeprom.cpp:31-46); Starship's update stalled 257 ms where its map
// saves, minutes after the disk was last used. Each pass leaves the disk idle, as the game does between saves, then
// writes BLOCKS blocks back to back, first as libultraship does, then each block in place with "r+b", which keeps the
// file's unit; the rounds leave it idle longer each time.

enum {
	SAVE_BYTES = 512,
	BLOCK_BYTES = 8,
	BLOCKS = 16,
	// The time base's rate as the kernel reports it; the console's is 49.875 MHz (docs/HARDWARE.md).
	TICKS_PER_US = 50,
};

static const char *const save_path = "probe_save.sav";
// Each round's idle before each of its passes.
static const unsigned idle_seconds[] = { 15, 60, 180 };

// libultraship's write of one block: the file read whole, made again and written whole.
static int write_remade(unsigned block, const unsigned char *bytes)
{
	unsigned char content[SAVE_BYTES] = { 0 };
	FILE *file = fopen(save_path, "rb");
	if (file) {
		(void)fread(content, 1, SAVE_BYTES, file);
		fclose(file);
	}
	memcpy(content + block * BLOCK_BYTES, bytes, BLOCK_BYTES);
	file = fopen(save_path, "wb");
	if (!file)
		return -1;
	const int written = fwrite(content, 1, SAVE_BYTES, file) == SAVE_BYTES ? 0 : -1;
	return fclose(file) == 0 ? written : -1;
}

// One block written where it lies in the file, which keeps its length and so its unit.
static int write_in_place(unsigned block, const unsigned char *bytes)
{
	FILE *const file = fopen(save_path, "r+b");
	if (!file)
		return -1;
	const int written =
		fseek(file, (long)(block * BLOCK_BYTES), SEEK_SET) == 0 && fwrite(bytes, 1, BLOCK_BYTES, file) == BLOCK_BYTES
		? 0
		: -1;
	return fclose(file) == 0 ? written : -1;
}

static void run_pass(const char *label, int (*write_block)(unsigned, const unsigned char *), unsigned round)
{
	const struct timespec idle = { .tv_sec = idle_seconds[round] };
	(void)nanosleep(&idle, NULL);
	uint32_t first_us = 0;
	uint32_t longest_us = 0;
	int failed = 0;
	const uint32_t start = probe_time_base();
	for (unsigned i = 0; i < BLOCKS; i++) {
		unsigned char bytes[BLOCK_BYTES];
		memset(bytes, (int)(round * BLOCKS + i), sizeof(bytes));
		const uint32_t block_start = probe_time_base();
		failed |= write_block(i, bytes);
		const uint32_t us = (probe_time_base() - block_start) / TICKS_PER_US;
		if (i == 0)
			first_us = us;
		if (us > longest_us)
			longest_us = us;
	}
	const uint32_t total_us = (probe_time_base() - start) / TICKS_PER_US;
	xsdk_log_write("save: %s, round %u: after %u s idle, %u blocks in %u us, the first %u us, the longest %u us%s",
		label, round, idle_seconds[round], (unsigned)BLOCKS, (unsigned)total_us, (unsigned)first_us,
		(unsigned)longest_us, failed ? "; a write failed" : "");
}

int main(void)
{
	// The file as the game leaves it, so both ways start from a 512-byte save.
	static const unsigned char zeros[SAVE_BYTES];
	FILE *const file = fopen(save_path, "wb");
	if (!file || fwrite(zeros, 1, SAVE_BYTES, file) != SAVE_BYTES || fclose(file) != 0) {
		xsdk_log_write("save: making %s failed", save_path);
		return 1;
	}
	for (unsigned round = 0; round < sizeof(idle_seconds) / sizeof(idle_seconds[0]); round++) {
		run_pass("remade whole, as libultraship writes", write_remade, round);
		run_pass("written in place", write_in_place, round);
	}
	(void)remove(save_path);
	xsdk_log_write("save: done");
	return 0;
}
