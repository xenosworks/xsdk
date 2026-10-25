#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>

#include "probe.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

// Milestone 61's probe: whether a file written as the log writes it, a line at a time, stalls where it grows into a new
// allocation unit, and whether space reserved when it's made, with NtCreateFile's allocation size, takes the stall
// away. Starship's log showed draw stalls of 34-370 ms each time it passed a 16 KB boundary. Three passes in game:, as
// the log's file: growing, then reserved, then growing again, so a cold cache can't explain a difference by itself.
// Then, as the log writes a line seconds apart, three passes, growing, reserved, and reserved over an old file as the
// log is made over the last run's, that leave the disk idle before each timed write: one inside a unit, one that starts
// the next unit. Each file is read back for its size, the space it holds and the disk's units it takes, open and
// closed, then removed.

enum {
	LINE_BYTES = 512,
	FILE_BYTES = 512 * 1024,
	// Twice what's written, to show whether closing gives back what wasn't used.
	RESERVE_BYTES = 2 * FILE_BYTES,
	LINES = FILE_BYTES / LINE_BYTES,
	UNIT_BYTES = 16 * 1024,
	// A write this long counts as a stall.
	STALL_US = 2000,
	MAX_STALLS = 12,
	// The time base's rate as the kernel reports it; the console's is 49.875 MHz (docs/HARDWARE.md).
	TICKS_PER_US = 50,
	IDLE_ROUNDS = 3,
	IDLE_SECONDS = 10,
};

static char line[LINE_BYTES];

static NTSTATUS create_file(HANDLE *handle, const char *path, const uint64_t *reserve_bytes)
{
	const ANSI_STRING name = {
		.length = (uint16_t)strlen(path), .maximum_length = (uint16_t)(strlen(path) + 1), .pointer = path
	};
	const OBJECT_ATTRIBUTES attributes = { .root_directory = OB_DOS_DEVICES, .name = &name };
	IO_STATUS_BLOCK io_status;
	// As src/log.c makes the log, with the reserve its only difference.
	return NtCreateFile(handle, GENERIC_WRITE | SYNCHRONIZE, &attributes, &io_status, reserve_bytes,
		FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OVERWRITE_IF,
		FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
}

// Writes a line at the file's end; returns how long it took, and the status through status.
static uint32_t timed_write(HANDLE handle, unsigned index, NTSTATUS *status)
{
	memset(line, 'a' + index % 26, sizeof(line));
	line[LINE_BYTES - 1] = '\n';
	IO_STATUS_BLOCK io_status;
	const uint32_t start = probe_time_base();
	*status = NtWriteFile(handle, 0, NULL, NULL, &io_status, line, LINE_BYTES, NULL);
	return (probe_time_base() - start) / TICKS_PER_US;
}

// The disk's free units, or 0 if it can't say.
static unsigned long long free_units(void)
{
	struct statvfs info;
	return statvfs("game:/", &info) == 0 ? (unsigned long long)info.f_bfree : 0;
}

// Logs the closed file's length, the space it holds and the units it takes from the disk, then removes it.
static void report_closed(const char *label, const char *path, unsigned long long free_before)
{
	FILE_NETWORK_OPEN_INFORMATION information;
	const ANSI_STRING name = {
		.length = (uint16_t)strlen(path), .maximum_length = (uint16_t)(strlen(path) + 1), .pointer = path
	};
	const OBJECT_ATTRIBUTES attributes = { .root_directory = OB_DOS_DEVICES, .name = &name };
	const NTSTATUS query_status = NtQueryFullAttributesFile(&attributes, &information);
	if (NT_SUCCESS(query_status)) {
		xsdk_log_write("growth: %s: closed, %llu bytes long, %llu held, %lld units taken", label,
			(unsigned long long)information.end_of_file, (unsigned long long)information.allocation_size,
			(long long)(free_before - free_units()));
	} else {
		xsdk_log_write("growth: %s: NtQueryFullAttributesFile failed: 0x%08X", label, (unsigned)query_status);
	}
	if (remove(path + strlen("game:\\")) != 0)
		xsdk_log_write("growth: %s: remove failed, errno %d", label, errno);
}

static void run_pass(const char *label, const char *path, const uint64_t *reserve_bytes)
{
	const unsigned long long free_before = free_units();
	HANDLE handle;
	const uint32_t create_start = probe_time_base();
	const NTSTATUS create_status = create_file(&handle, path, reserve_bytes);
	const uint32_t create_us = (probe_time_base() - create_start) / TICKS_PER_US;
	if (!NT_SUCCESS(create_status)) {
		xsdk_log_write("growth: %s: NtCreateFile(%s) failed: 0x%08X", label, path, (unsigned)create_status);
		return;
	}
	uint32_t stall_offsets[MAX_STALLS];
	uint32_t stall_us[MAX_STALLS];
	unsigned stalls = 0;
	unsigned at_units = 0;
	uint32_t longest_us = 0;
	uint64_t total_us = 0;
	NTSTATUS write_status = STATUS_SUCCESS;
	for (unsigned i = 0; i < LINES && NT_SUCCESS(write_status); i++) {
		const uint32_t us = timed_write(handle, i, &write_status);
		total_us += us;
		if (us > longest_us)
			longest_us = us;
		if (us >= STALL_US) {
			// The offset the write started at; a stall at a multiple of the unit is where the file grew.
			if (stalls < MAX_STALLS) {
				stall_offsets[stalls] = i * LINE_BYTES;
				stall_us[stalls] = us;
			}
			stalls++;
			if (i * LINE_BYTES % UNIT_BYTES == 0)
				at_units++;
		}
	}
	(void)NtClose(handle);
	char list[MAX_STALLS * 24] = "";
	for (unsigned i = 0; i < stalls && i < MAX_STALLS; i++) {
		const size_t used = strlen(list);
		(void)snprintf(list + used, sizeof(list) - used, "%s%u KB %u us", i ? ", " : "",
			(unsigned)(stall_offsets[i] / 1024), (unsigned)stall_us[i]);
	}
	xsdk_log_write(
		"growth: %s: made in %u us; %u writes of %u bytes, 0x%08X, %llu us in all, the longest %u us; %u "
		"of %u us or more, %u of them at a 16 KB boundary: %s",
		label, (unsigned)create_us, (unsigned)LINES, (unsigned)LINE_BYTES, (unsigned)write_status,
		(unsigned long long)total_us, (unsigned)longest_us, stalls, (unsigned)STALL_US, at_units, list);
	report_closed(label, path, free_before);
}

// Each round fills the file to the middle of a unit, leaves the disk idle and times a write there, then fills it to the
// unit's end, leaves it idle and times the write that starts the next unit.
static void run_idle_pass(const char *label, const char *path, const uint64_t *reserve_bytes)
{
	const unsigned long long free_before = free_units();
	HANDLE handle;
	const NTSTATUS create_status = create_file(&handle, path, reserve_bytes);
	if (!NT_SUCCESS(create_status)) {
		xsdk_log_write("growth: %s: NtCreateFile(%s) failed: 0x%08X", label, path, (unsigned)create_status);
		return;
	}
	xsdk_log_write("growth: %s: made, %lld units taken", label, (long long)(free_before - free_units()));
	static const struct timespec idle = { .tv_sec = IDLE_SECONDS };
	NTSTATUS status = STATUS_SUCCESS;
	unsigned index = 0;
	for (unsigned round = 0; round < IDLE_ROUNDS && NT_SUCCESS(status); round++) {
		while (NT_SUCCESS(status) && index * LINE_BYTES < round * UNIT_BYTES + UNIT_BYTES / 2)
			(void)timed_write(handle, index++, &status);
		(void)nanosleep(&idle, NULL);
		const uint32_t inside_us = timed_write(handle, index++, &status);
		while (NT_SUCCESS(status) && index * LINE_BYTES < (round + 1) * UNIT_BYTES)
			(void)timed_write(handle, index++, &status);
		(void)nanosleep(&idle, NULL);
		const unsigned unit_kb = index * LINE_BYTES / 1024;
		const uint32_t unit_us = timed_write(handle, index++, &status);
		xsdk_log_write(
			"growth: %s: after %u s idle, a write inside a unit took %u us, the write starting the unit at "
			"%u KB %u us; 0x%08X",
			label, (unsigned)IDLE_SECONDS, (unsigned)inside_us, unit_kb, (unsigned)unit_us, (unsigned)status);
	}
	(void)NtClose(handle);
	report_closed(label, path, free_before);
}

int main(void)
{
	struct statvfs info;
	if (statvfs("game:/", &info) == 0) {
		xsdk_log_write("growth: game: has %lu-byte units, %llu of %llu free", info.f_bsize,
			(unsigned long long)info.f_bfree, (unsigned long long)info.f_blocks);
	} else {
		xsdk_log_write("growth: statvfs(game:) failed, errno %d", errno);
	}
	static const uint64_t reserve_bytes = RESERVE_BYTES;
	run_pass("growing", "game:\\probe_growth_1.bin", NULL);
	run_pass("reserved", "game:\\probe_growth_2.bin", &reserve_bytes);
	run_pass("growing again", "game:\\probe_growth_3.bin", NULL);
	run_idle_pass("growing, idle", "game:\\probe_growth_4.bin", NULL);
	run_idle_pass("reserved, idle", "game:\\probe_growth_5.bin", &reserve_bytes);
	FILE *const old = fopen("probe_growth_6.bin", "wb");
	if (old) {
		for (unsigned i = 0; i < 2 * UNIT_BYTES / LINE_BYTES; i++)
			(void)fwrite(line, 1, LINE_BYTES, old);
		(void)fclose(old);
	}
	run_idle_pass("reserved over an old file, idle", "game:\\probe_growth_6.bin", &reserve_bytes);
	xsdk_log_write("growth: done");
	return 0;
}
