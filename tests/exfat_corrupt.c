#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"
#include "xsdk/thread.h"

// Milestone 77 step 3 (docs/scope/77-exfat.md): damages, or mends, the exFAT boot sector of the first USB stick, for
// the check that a damaged volume is refused with an error and not mounted. Byte 0x20 of the boot sector lies in its
// MustBeZero field, under the boot region's checksum (exFAT specification, 3.1.2 and 3.1.3): a 0 there becomes 0xFF,
// which breaks the checksum; anything else becomes 0 again. A loaded module is asked to unload first, through its
// exfat_unload.txt, so nothing holds the volume while the sector changes; exfat_load.xex loads it again afterwards.
// The sector goes through read and write IRPs to the device object, as step 1 wrote the stick. Kernel 17559 only.

enum {
	KERNEL_BUILD = 17559,
	SPLASH_MS = 1000,
	SECTOR_BYTES_MAX = 4096,
	IRP_MJ_READ = 2,
	IRP_MJ_WRITE = 3,
	IOCTL_DISK_GET_DRIVE_GEOMETRY = 0x70000,
	MBR_PARTITION_TABLE = 446,
	MBR_TYPE_EXFAT = 0x07,
	BOOT_NAME = 3,
	BOOT_MUST_BE_ZERO_BYTE = 0x20,
	UNLOAD_WAIT_MS = 5000,
	UNLOAD_POLL_MS = 100,
};
static const char DEVICE_PATH[] = "\\Device\\Mass0";
static const char MODULE_NAME[] = "exfat.xex";
static const char UNLOAD_PATH[] = "\\Device\\Harddisk0\\Partition1\\XSDK\\exfat\\exfat_unload.txt";

static _Alignas(4096) uint8_t sector[SECTOR_BYTES_MAX];

static ANSI_STRING ansi(const char *text)
{
	const ANSI_STRING string = { (uint16_t)strlen(text), (uint16_t)(strlen(text) + 1), text };
	return string;
}

static uint32_t le32(const uint8_t *bytes)
{
	return bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static bool module_loaded(void)
{
	HANDLE module;
	return XexGetModuleHandle(MODULE_NAME, &module) == 0;
}

// Asks the module to unload and waits for it to go.
static bool unload_module(void)
{
	const ANSI_STRING name = ansi(UNLOAD_PATH);
	const OBJECT_ATTRIBUTES attributes = { 0, &name, 0 };
	IO_STATUS_BLOCK io;
	HANDLE file;
	const NTSTATUS status = NtCreateFile(&file, GENERIC_WRITE | SYNCHRONIZE, &attributes, &io, NULL,
		FILE_ATTRIBUTE_NORMAL, 0, FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("exfat_corrupt: creating exfat_unload.txt failed: 0x%08X", (unsigned)status);
		return false;
	}
	(void)NtClose(file);
	for (unsigned waited = 0; waited < UNLOAD_WAIT_MS; waited += UNLOAD_POLL_MS) {
		if (!module_loaded()) {
			xsdk_log_write("exfat_corrupt: the module unloaded after %u ms", waited);
			return true;
		}
		(void)xsdk_thread_sleep(UNLOAD_POLL_MS);
	}
	xsdk_log_write("exfat_corrupt: the module is still loaded after %u ms", UNLOAD_WAIT_MS);
	return false;
}

static bool transfer(uint32_t device, uint32_t major, uint64_t offset, uint32_t bytes)
{
	const NTSTATUS status = IoSynchronousFsdRequest(major, device, sector, bytes, &offset);
	if (!NT_SUCCESS(status))
		xsdk_log_write("exfat_corrupt: %s at %u failed: 0x%08X", major == IRP_MJ_READ ? "read" : "write",
			(unsigned)offset, (unsigned)status);
	return NT_SUCCESS(status);
}

int main(void)
{
	(void)xsdk_splash_show(SPLASH_MS);
	xsdk_log_write("exfat_corrupt: milestone 77 step 3, kernel %u.%u.%u.%u", XboxKrnlVersion->major,
		XboxKrnlVersion->minor, XboxKrnlVersion->build, XboxKrnlVersion->qfe);
	if (XboxKrnlVersion->build != KERNEL_BUILD) {
		xsdk_log_write("exfat_corrupt: the device layouts are kernel %u's; not touching the stick", KERNEL_BUILD);
		return 1;
	}
	if (module_loaded() && !unload_module())
		return 1;
	const ANSI_STRING name = ansi(DEVICE_PATH);
	uint32_t device = 0;
	NTSTATUS status = ObReferenceObjectByName(&name, 0, IoDeviceObjectType, NULL, &device);
	if (!NT_SUCCESS(status) || !device) {
		xsdk_log_write("exfat_corrupt: %s: 0x%08X; no stick", DEVICE_PATH, (unsigned)status);
		return 1;
	}
	uint32_t geometry[2] = { 0, 0 };
	status = IoSynchronousDeviceIoControlRequest(
		IOCTL_DISK_GET_DRIVE_GEOMETRY, device, NULL, 0, geometry, sizeof geometry, NULL);
	const uint32_t sector_bytes = geometry[1];
	if (!NT_SUCCESS(status) || sector_bytes < 512 || sector_bytes > SECTOR_BYTES_MAX) {
		xsdk_log_write("exfat_corrupt: geometry 0x%08X, %u bytes a sector; no media", (unsigned)status, sector_bytes);
		(void)ObDereferenceObject(device);
		return 1;
	}
	int result = 1;
	uint64_t boot = 0;
	uint8_t was = 0;
	if (!transfer(device, IRP_MJ_READ, 0, sector_bytes))
		goto done;
	if (sector[510] == 0x55 && sector[511] == 0xAA && sector[MBR_PARTITION_TABLE + 4] == MBR_TYPE_EXFAT)
		boot = (uint64_t)le32(sector + MBR_PARTITION_TABLE + 8) * sector_bytes;
	if (boot && !transfer(device, IRP_MJ_READ, boot, sector_bytes))
		goto done;
	if (memcmp(sector + BOOT_NAME, "EXFAT   ", 8) != 0) {
		xsdk_log_write("exfat_corrupt: no exFAT boot sector at %u; leaving the stick alone", (unsigned)boot);
		goto done;
	}
	was = sector[BOOT_MUST_BE_ZERO_BYTE];
	sector[BOOT_MUST_BE_ZERO_BYTE] = was ? 0 : 0xFF;
	if (!transfer(device, IRP_MJ_WRITE, boot, sector_bytes))
		goto done;
	memset(sector, 0, sector_bytes);
	if (!transfer(device, IRP_MJ_READ, boot, sector_bytes))
		goto done;
	const bool kept = sector[BOOT_MUST_BE_ZERO_BYTE] == (was ? 0 : 0xFF);
	xsdk_log_write("RESULT boot sector at %u byte 0x20: %02X -> %02X, read back %s; the volume is now %s",
		(unsigned)boot, was, was ? 0 : 0xFF, kept ? "the same" : "DIFFERENT", was ? "mended" : "damaged");
	result = kept ? 0 : 1;
done:
	(void)ObDereferenceObject(device);
	return result;
}
