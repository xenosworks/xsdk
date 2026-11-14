#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"

// Milestone 77 step 1 (docs/scope/77-exfat.md): writes a disk image's head onto the first USB stick. exfat_image.bin
// beside the title holds a disk's first bytes as Windows formatted one of the stick's size: an MBR whose first
// partition is exFAT, then everything the format wrote. The title refuses unless the image describes an exFAT
// volume that fits the stick, dismounts the volume the kernel has on \Device\Mass0 (the FAT driver's, until now),
// writes the image through write IRPs to the device object (plugins/exfat/kernel_seam.md, section 2), reads it back
// and compares. Kernel 17559 only, for the device object's mounted-volume word.

enum {
	KERNEL_BUILD = 17559,
	SPLASH_MS = 2000,
	SECTOR_BYTES = 512,
	CHUNK_BYTES = 65536,
	IMAGE_BYTES_MAX = 32 << 20,
	// Sectors left after the partition before the image is judged built for another stick.
	SLACK_SECTORS_MAX = 4096,
	IRP_MJ_READ = 2,
	IRP_MJ_WRITE = 3,
	IOCTL_DISK_GET_DRIVE_GEOMETRY = 0x70000,
	MBR_PARTITION_TABLE = 446,
	MBR_TYPE_EXFAT = 0x07,
};
static const char IMAGE_PATH[] = "exfat_image.bin";
static const char DEVICE_PATH[] = "\\Device\\Mass0";

static unsigned passed_count;
static unsigned failed_count;

static void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	passed_count += passed;
	failed_count += !passed;
}

static uint32_t kernel_word(uint32_t address)
{
	return *(volatile const uint32_t *)(uintptr_t)address;
}

static uint32_t le32(const uint8_t *bytes)
{
	return bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static void log_mounted(const char *when, uint32_t device)
{
	const uint32_t volume = kernel_word(device + 12);
	xsdk_log_write("RESULT mounted %s: volume %08X driver %08X", when, (unsigned)volume,
		volume ? (unsigned)kernel_word(volume + 8) : 0);
}

// The image beside the title, in physically contiguous memory; returns its size, 0 when it can't be used.
static uint32_t load_image(uint8_t **image)
{
	FILE *const file = fopen(IMAGE_PATH, "rb");
	if (!file) {
		xsdk_log_write("image: %s is not beside the title", IMAGE_PATH);
		return 0;
	}
	fseek(file, 0, SEEK_END);
	const long size = ftell(file);
	fseek(file, 0, SEEK_SET);
	if (size <= 0 || size > IMAGE_BYTES_MAX || size % SECTOR_BYTES) {
		xsdk_log_write("image: %ld bytes; it must be whole sectors, at most %u bytes", size, IMAGE_BYTES_MAX);
		fclose(file);
		return 0;
	}
	*image = MmAllocatePhysicalMemoryEx(0, (uint32_t)size, PAGE_READWRITE, 0, UINT32_MAX, 0);
	const size_t read = *image ? fread(*image, 1, (size_t)size, file) : 0;
	fclose(file);
	if (read != (size_t)size) {
		xsdk_log_write("image: read %u of %ld bytes", (unsigned)read, size);
		return 0;
	}
	return (uint32_t)size;
}

// Checks the MBR's first partition and its exFAT boot sector; returns the partition's end sector, 0 on a failure.
static uint64_t validate_image(const uint8_t *image, uint32_t size)
{
	const uint8_t *const entry = image + MBR_PARTITION_TABLE;
	const uint32_t start = le32(entry + 8);
	const uint32_t count = le32(entry + 12);
	xsdk_log_write("RESULT image: %u bytes, MBR entry 0 type %02X start %u sectors %u", size, entry[4], (unsigned)start,
		(unsigned)count);
	check("image: the MBR ends in 0x55 0xAA", image[510] == 0x55 && image[511] == 0xAA);
	check("image: the first partition is type 07", entry[4] == MBR_TYPE_EXFAT);
	check("image: the image holds the partition's boot sector", ((uint64_t)start + 1) * SECTOR_BYTES <= size);
	if (failed_count)
		return 0;
	// The main boot sector's fields (Microsoft's exFAT specification, section 3.1), little-endian.
	const uint8_t *const boot = image + start * SECTOR_BYTES;
	const uint64_t volume_length = le32(boot + 72) | (uint64_t)le32(boot + 76) << 32;
	xsdk_log_write(
		"RESULT image: boot sector \"%.8s\", volume length %u sectors, %u bytes a sector, %u sectors a "
		"cluster, %u clusters, serial %08X",
		(const char *)boot + 3, (unsigned)volume_length, 1u << boot[108], 1u << boot[109], (unsigned)le32(boot + 92),
		(unsigned)le32(boot + 100));
	check("image: the partition's boot sector is exFAT",
		memcmp(boot + 3, "EXFAT   ", 8) == 0 && boot[510] == 0x55 && boot[511] == 0xAA);
	check("image: the volume's length is the partition's", volume_length == count);
	check("image: the volume's sectors are 512 bytes", boot[108] == 9);
	return failed_count ? 0 : (uint64_t)start + count;
}

static uint32_t transfer(uint32_t major, uint32_t device, uint8_t *bytes, uint32_t size, const char *what)
{
	uint32_t done = 0;
	NTSTATUS status = 0;
	while (done < size && NT_SUCCESS(status)) {
		const uint32_t chunk = size - done < CHUNK_BYTES ? size - done : CHUNK_BYTES;
		const uint64_t offset = done;
		status = IoSynchronousFsdRequest(major, device, bytes + done, chunk, &offset);
		if (NT_SUCCESS(status))
			done += chunk;
	}
	xsdk_log_write(
		"RESULT %s: %u of %u bytes, last status %08X", what, (unsigned)done, (unsigned)size, (unsigned)status);
	return done;
}

static void write_image(uint32_t device, uint8_t *image, uint32_t size)
{
	uint8_t *const readback = MmAllocatePhysicalMemoryEx(0, size, PAGE_READWRITE, 0, UINT32_MAX, 0);
	check("write: the read-back buffer allocated", readback != NULL);
	if (!readback)
		return;
	check("write: the whole image written", transfer(IRP_MJ_WRITE, device, image, size, "write") == size);
	memset(readback, 0, size);
	check("write: the stick reads back the image",
		transfer(IRP_MJ_READ, device, readback, size, "read back") == size && memcmp(image, readback, size) == 0);
}

int main(void)
{
	xsdk_log_write("probe_exfat_write: kernel %u.%u.%u.%u", XboxKrnlVersion->major, XboxKrnlVersion->minor,
		XboxKrnlVersion->build, XboxKrnlVersion->qfe);
	if (XboxKrnlVersion->build != KERNEL_BUILD) {
		xsdk_log_write("probe_exfat_write: the device object's layout is kernel 17559's; nothing runs");
		return 1;
	}
	check("splash: the vulkan4xenon logo shown", NT_SUCCESS(xsdk_splash_show(SPLASH_MS)));

	uint8_t *image = NULL;
	const uint32_t size = load_image(&image);
	check("image: exfat_image.bin loaded", size != 0);
	const uint64_t end = size ? validate_image(image, size) : 0;
	if (end) {
		const ANSI_STRING name = { sizeof(DEVICE_PATH) - 1, sizeof(DEVICE_PATH), DEVICE_PATH };
		uint32_t device = 0;
		NTSTATUS status = ObReferenceObjectByName(&name, 0, IoDeviceObjectType, NULL, &device);
		check("mass0: referenced by name", NT_SUCCESS(status));
		if (NT_SUCCESS(status)) {
			uint32_t geometry[2] = { 0, 0 };
			uint32_t returned = 0;
			status = IoSynchronousDeviceIoControlRequest(
				IOCTL_DISK_GET_DRIVE_GEOMETRY, device, NULL, 0, geometry, sizeof(geometry), &returned);
			xsdk_log_write("RESULT geometry: status %08X sectors %u bytes a sector %u; the partition ends at %u",
				(unsigned)status, (unsigned)geometry[0], (unsigned)geometry[1], (unsigned)end);
			check("mass0: 512-byte sectors", NT_SUCCESS(status) && geometry[1] == SECTOR_BYTES);
			check("mass0: the partition ends within the stick's last sectors",
				NT_SUCCESS(status) && end <= geometry[0] && geometry[0] - end < SLACK_SECTORS_MAX);
			log_mounted("before", device);
			if (!failed_count) {
				status = IoDismountVolumeByName(&name);
				xsdk_log_write("RESULT dismount: status %08X", (unsigned)status);
				log_mounted("after the dismount", device);
				check("mass0: no volume attached after the dismount", kernel_word(device + 12) == 0);
			}
			if (!failed_count)
				write_image(device, image, size);
			log_mounted("after writing", device);
			ObDereferenceObject(device);
		}
	}
	xsdk_log_write("probe_exfat_write: %u passed, %u failed", passed_count, failed_count);
	return failed_count != 0;
}
