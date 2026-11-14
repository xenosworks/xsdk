#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"

// Milestone 77 step 3 (docs/scope/77-exfat.md): the exFAT module's file writes against a title's raw 64 KB IRPs to
// the stick, over 2 GiB, as the 4 GB file of exfat_check ran at 15 MiB/s for its first GiB and 11 after
// (results/2026-11-14-milestone-77-step-3/check_final). A 2 GiB file through the module, then 2 GiB of raw writes at
// the stick's tail, each timed every 512 MiB. The module's writes ran at 9 MiB/s against 15 raw until its poll
// stopped waiting for the lock a stream holds (speed7 to speed15). Kernel 17559 only.

enum {
	KERNEL_BUILD = 17559,
	SPLASH_MS = 1000,
	IRP_MJ_READ = 2,
	IRP_MJ_WRITE = 3,
	IOCTL_DISK_GET_DRIVE_GEOMETRY = 0x70000,
	REGION_MIB = 2048,
	PROGRESS_MIB = 512,
	BUFFER_BYTES = 1 << 20,
	IRP_BYTES = 64 << 10,
	// Sectors kept clear at the stick's end, past the partition's last whole megabyte.
	TAIL_SECTORS = 4096,
	MBR_PARTITION_TABLE = 446,
	MBR_TYPE_EXFAT = 0x07,
	BOOT_HEAP_OFFSET = 0x58,
};
static const char DEVICE_PATH[] = "\\Device\\Mass0";
static const char FILE_PATH[] = "usb0:/exfat_check/speed.bin";

static _Alignas(4096) uint8_t buffer[BUFFER_BYTES];
static _Alignas(4096) uint8_t sector[4096];
static unsigned passed_count;
static unsigned failed_count;

static void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	passed_count += passed;
	failed_count += !passed;
}

static uint64_t now_ms(void)
{
	struct timespec time;
	(void)clock_gettime(CLOCK_MONOTONIC, &time);
	return (uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000;
}

static uint32_t le32(const uint8_t *bytes)
{
	return bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static uint32_t pattern_word(uint32_t piece, uint32_t j)
{
	return (j * 0x01010101u) ^ (piece * 0x7F4A7C15u);
}

static void fill(uint32_t piece)
{
	uint32_t *const words = (uint32_t *)buffer;
	for (uint32_t j = 0; j < BUFFER_BYTES / 4; j++)
		words[j] = pattern_word(piece, j);
}

static uint64_t rate(uint32_t mib, uint64_t elapsed_ms)
{
	return (uint64_t)mib * 1000 / (elapsed_ms ? elapsed_ms : 1);
}

static void progress(const char *what, uint32_t mib, uint64_t start)
{
	if (mib && mib % PROGRESS_MIB == 0) {
		const uint64_t elapsed = now_ms() - start;
		xsdk_log_write("  %s: %u MiB in %llu ms, %llu MiB/s", what, mib, (unsigned long long)elapsed,
			(unsigned long long)rate(mib, elapsed));
	}
}

// REGION_MIB at region in IRP_BYTES writes; the time in ms, 0 on a failure.
static uint64_t raw_sweep(uint32_t device, uint64_t region)
{
	const uint64_t start = now_ms();
	for (uint64_t done = 0; done < (uint64_t)REGION_MIB << 20; done += IRP_BYTES) {
		const uint32_t within = (uint32_t)(done % BUFFER_BYTES);
		if (within == 0) {
			fill((uint32_t)(done / BUFFER_BYTES));
			progress("raw", (uint32_t)(done >> 20), start);
		}
		const uint64_t offset = region + done;
		const NTSTATUS status = IoSynchronousFsdRequest(IRP_MJ_WRITE, device, buffer + within, IRP_BYTES, &offset);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write(
				"  raw write of %u at %llu: 0x%08X", IRP_BYTES, (unsigned long long)offset, (unsigned)status);
			return 0;
		}
	}
	const uint64_t elapsed = now_ms() - start;
	return elapsed ? elapsed : 1;
}

// REGION_MIB into a new file through xsdk's write, unlinked after; the time in ms, 0 on a failure (logged).
static uint64_t file_sweep(void)
{
	const int fd = open(FILE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) {
		xsdk_log_write("  open(%s) failed, errno %d", FILE_PATH, errno);
		return 0;
	}
	const uint64_t start = now_ms();
	bool written = true;
	for (uint32_t piece = 0; written && piece < REGION_MIB; piece++) {
		fill(piece);
		progress("the file", piece, start);
		written = write(fd, buffer, BUFFER_BYTES) == BUFFER_BYTES;
	}
	const uint64_t elapsed = now_ms() - start;
	if (!written)
		xsdk_log_write("  write failed, errno %d", errno);
	(void)close(fd);
	(void)unlink(FILE_PATH);
	return written ? (elapsed ? elapsed : 1) : 0;
}

int main(void)
{
	(void)xsdk_splash_show(SPLASH_MS);
	xsdk_log_write("probe_mass_speed: milestone 77 step 3, kernel %u.%u.%u.%u", XboxKrnlVersion->major,
		XboxKrnlVersion->minor, XboxKrnlVersion->build, XboxKrnlVersion->qfe);
	if (XboxKrnlVersion->build != KERNEL_BUILD) {
		xsdk_log_write("probe_mass_speed: the device layouts are kernel %u's; not touching the stick", KERNEL_BUILD);
		return 1;
	}
	const ANSI_STRING name = { sizeof DEVICE_PATH - 1, sizeof DEVICE_PATH, DEVICE_PATH };
	uint32_t device = 0;
	NTSTATUS status = ObReferenceObjectByName(&name, 0, IoDeviceObjectType, NULL, &device);
	if (!NT_SUCCESS(status) || !device) {
		xsdk_log_write("probe_mass_speed: %s: 0x%08X; no stick", DEVICE_PATH, (unsigned)status);
		return 1;
	}
	uint32_t geometry[2] = { 0, 0 };
	status = IoSynchronousDeviceIoControlRequest(
		IOCTL_DISK_GET_DRIVE_GEOMETRY, device, NULL, 0, geometry, sizeof geometry, NULL);
	if (!NT_SUCCESS(status) || geometry[1] != 512) {
		xsdk_log_write("probe_mass_speed: geometry 0x%08X, %u bytes a sector; stopping", (unsigned)status, geometry[1]);
		(void)ObDereferenceObject(device);
		return 1;
	}
	// The cluster heap's start: the MBR's partition, then the boot sector's ClusterHeapOffset.
	const uint64_t zero = 0;
	uint64_t partition = 0;
	uint64_t heap = 0;
	if (NT_SUCCESS(IoSynchronousFsdRequest(IRP_MJ_READ, device, sector, 512, &zero)) && sector[510] == 0x55 &&
		sector[MBR_PARTITION_TABLE + 4] == MBR_TYPE_EXFAT) {
		partition = (uint64_t)le32(sector + MBR_PARTITION_TABLE + 8) * 512;
		if (NT_SUCCESS(IoSynchronousFsdRequest(IRP_MJ_READ, device, sector, 512, &partition)) &&
			memcmp(sector + 3, "EXFAT   ", 8) == 0)
			heap = partition + (uint64_t)le32(sector + BOOT_HEAP_OFFSET) * 512;
	}
	if (!heap) {
		xsdk_log_write("probe_mass_speed: no exFAT partition found; stopping");
		(void)ObDereferenceObject(device);
		return 1;
	}
	const uint64_t tail = ((uint64_t)geometry[0] - TAIL_SECTORS) * 512 - ((uint64_t)REGION_MIB << 20);
	xsdk_log_write(
		"probe_mass_speed: heap at %llu, the tail region at %llu", (unsigned long long)heap, (unsigned long long)tail);

	const uint64_t file_ms = file_sweep();
	xsdk_log_write("RESULT file write through the module: %u MiB in %llu ms, %llu MiB/s", REGION_MIB,
		(unsigned long long)file_ms, (unsigned long long)rate(REGION_MIB, file_ms));
	check("the file write succeeds", file_ms != 0);
	const uint64_t ms = raw_sweep(device, tail);
	xsdk_log_write("RESULT raw 64 KB writes at the tail: %u MiB in %llu ms, %llu MiB/s", REGION_MIB,
		(unsigned long long)ms, (unsigned long long)rate(REGION_MIB, ms));
	check("the raw sweep succeeds", ms != 0);
	(void)ObDereferenceObject(device);
	xsdk_log_write("tests: %u passed, %u failed", passed_count, failed_count);
	return failed_count ? 1 : 0;
}
