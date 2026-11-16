#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"

// Milestone 78 step 1 (docs/scope/78-exfat-hdd.md): reads the drive's security sector, byte offset 0x2000 of
// \Device\Harddisk0\Partition0 (the whole disk), the "Josh" sector the dash checks before it lists a drive as
// formatted, and logs its fields and a hex dump of its head. Nothing else is read, nothing is written. For the
// question whether the dash's "unformatted" verdict on the exFAT drive is the sector's or the file system's
// (results/2026-11-15-milestone-78-step-1).

enum { SPLASH_MS = 2000, SECTOR_OFFSET = 0x2000, READ_BYTES = 4096, DUMP_BYTES = 0x170, IRP_MJ_READ = 2 };
enum { IOCTL_DISK_GET_DRIVE_GEOMETRY = 0x70000 };
// The sector's layout as the scene's tools write hddss.bin: magic, the drive's serial, firmware and model as ATA
// identify gives them, the MS logo's hash, the sector count, the RSA signature, then the logo.
enum {
	MAGIC = 0x000,
	SERIAL = 0x004,
	SERIAL_BYTES = 20,
	FIRMWARE = 0x018,
	FIRMWARE_BYTES = 8,
	MODEL = 0x020,
	MODEL_BYTES = 40,
	SECTORS = 0x05C,
	SIGNATURE = 0x060,
	LOGO_SIZE = 0x160,
};

static const char *const DISK = "\\Device\\Harddisk0\\Partition0";

static uint32_t le32(const uint8_t *bytes)
{
	return bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static uint32_t be32(const uint8_t *bytes)
{
	return (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
}

// A fixed-width ATA string field, unprintable bytes as dots.
static void text_field(const char *name, const uint8_t *bytes, unsigned count)
{
	char text[MODEL_BYTES + 1];
	for (unsigned i = 0; i < count; i++)
		text[i] = bytes[i] >= 0x20 && bytes[i] < 0x7F ? (char)bytes[i] : '.';
	text[count] = 0;
	xsdk_log_write("RESULT %s: \"%s\"", name, text);
}

static void dump(const uint8_t *bytes, unsigned count)
{
	for (unsigned start = 0; start < count; start += 16) {
		char line[16 * 3 + 1] = "";
		for (unsigned i = start; i < start + 16; i++)
			snprintf(line + strlen(line), sizeof(line) - strlen(line), "%02X ", bytes[i]);
		xsdk_log_write("sector %03X: %s", start, line);
	}
}

int main(void)
{
	xsdk_log_write("probe_hdd_security: kernel %u.%u.%u.%u", XboxKrnlVersion->major, XboxKrnlVersion->minor,
		XboxKrnlVersion->build, XboxKrnlVersion->qfe);
	(void)xsdk_splash_show(SPLASH_MS);

	const ANSI_STRING name = { (uint16_t)strlen(DISK), (uint16_t)(strlen(DISK) + 1), DISK };
	uint32_t object = 0;
	NTSTATUS status = ObReferenceObjectByName(&name, 0, IoDeviceObjectType, NULL, &object);
	if (!NT_SUCCESS(status) || !object) {
		xsdk_log_write("RESULT %s: reference failed, 0x%08X", DISK, (unsigned)status);
		return 1;
	}
	uint32_t geometry[2] = { 0, 0 };
	status = IoSynchronousDeviceIoControlRequest(
		IOCTL_DISK_GET_DRIVE_GEOMETRY, object, NULL, 0, geometry, sizeof geometry, NULL);
	xsdk_log_write("RESULT %s: geometry 0x%08X, %u sectors of %u", DISK, (unsigned)status, (unsigned)geometry[0],
		(unsigned)geometry[1]);

	// A read IRP wants a physically contiguous buffer, as the formatter's bounce buffer is.
	uint8_t *const bounce = MmAllocatePhysicalMemoryEx(0, READ_BYTES, PAGE_READWRITE, 0, UINT32_MAX, 0);
	if (!bounce) {
		xsdk_log_write("RESULT no memory for the read buffer");
		return 1;
	}
	uint64_t offset = SECTOR_OFFSET;
	status = IoSynchronousFsdRequest(IRP_MJ_READ, object, bounce, READ_BYTES, &offset);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("RESULT read of %u bytes at 0x%X failed: 0x%08X", READ_BYTES, SECTOR_OFFSET, (unsigned)status);
		return 1;
	}
	bool zero = true;
	for (unsigned i = 0; i < 512 && zero; i++)
		zero = bounce[i] == 0;
	const bool josh = memcmp(bounce + MAGIC, "Josh", 4) == 0;
	xsdk_log_write("RESULT sector at 0x%X: %s", SECTOR_OFFSET,
		josh       ? "the Josh magic"
			: zero ? "all zero, no security sector"
				   : "no Josh magic");
	if (josh) {
		text_field("serial", bounce + SERIAL, SERIAL_BYTES);
		text_field("firmware", bounce + FIRMWARE, FIRMWARE_BYTES);
		text_field("model", bounce + MODEL, MODEL_BYTES);
		xsdk_log_write("RESULT sectors: %u little-endian, %u big-endian (the drive has %u)",
			(unsigned)le32(bounce + SECTORS), (unsigned)be32(bounce + SECTORS), (unsigned)geometry[0]);
		xsdk_log_write("RESULT logo size: %u little-endian, %u big-endian", (unsigned)le32(bounce + LOGO_SIZE),
			(unsigned)be32(bounce + LOGO_SIZE));
		bool signed_ = false;
		for (unsigned i = SIGNATURE; i < LOGO_SIZE && !signed_; i++)
			signed_ = bounce[i] != 0;
		xsdk_log_write("RESULT signature: %s", signed_ ? "present (non-zero bytes)" : "all zero");
	}
	dump(bounce, DUMP_BYTES);
	(void)ObDereferenceObject(object);
	return 0;
}
