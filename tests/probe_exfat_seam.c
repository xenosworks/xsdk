#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"

// Milestone 77 step 1 (docs/scope/77-exfat.md): the console's side of plugins/exfat/kernel_seam.md. On kernel 17559
// only: checks the kernel in memory is the image the note was read from, references \Device\Mass0 by name, logs its
// device object and its driver's table against the note's layouts, asks the geometry and partition IOCTLs, reads
// sector 0 through a read IRP into sector0.bin beside the title (and the partition's first sector into boot.bin when
// sector 0 is an MBR), then opens the device's root, the device itself and the root again as a title's file calls
// would, recording each status and whether a volume attached. Nothing is written to the stick.

enum { KERNEL_BUILD = 17559, SPLASH_MS = 2000, SECTOR_BYTES_MAX = 4096, DEVICE_OBJECT_BYTES = 80, DRIVER_WORDS = 14 };

// Kernel 17559 (plugins/exfat/kernel_seam.md).
enum {
	MASS_DEVICE_TYPE = 45,
	IRP_MJ_READ = 2,
	// NT's public values.
	IOCTL_DISK_GET_DRIVE_GEOMETRY = 0x70000,
	IOCTL_DISK_GET_PARTITION_INFO = 0x74004,
	MBR_PARTITION_TABLE = 446,
	MBR_PARTITION_ENTRY_BYTES = 16,
};
static const uint32_t KERNEL_IO_DEVICE_OBJECT_TYPE = 0x80040B08;
static const uint32_t KERNEL_FAT_DRIVER = 0x80042F04;
static const uint32_t KERNEL_HFS_DRIVER = 0x8004331C;
static const uint32_t KERNEL_RAW_DRIVER = 0x80043224;
static const uint32_t KERNEL_MASS_DRIVER = 0x80044B10;
static const uint32_t KERNEL_BOOT_HOOK = 0x80061380;
static const uint32_t KERNEL_MOUNT_CHAIN = 0x8006C044;
static const uint32_t KERNEL_TEXT_PADDING = 0x801616C8;
static const uint32_t KERNEL_TEXT_PADDING_END = 0x8016FFFC;
static const uint32_t KERNEL_IO_SPIN_LOCK = 0x801A5618;

// The image's words at those addresses (kernel_seam.md, sections 1, 6 and 7).
static const uint32_t boot_hook_words[] = { 0x38600079, 0x4BFFFC7D, 0x48006121, 0x7C7E1B79 };
static const uint32_t mount_chain_words[] = { 0x7FC3F378, 0x480338D1, 0x7C7F1B78, 0x7F1FE800, 0x409A005C, 0x7FC3F378,
	0x4804A91D, 0x7C7F1B78, 0x7F1FE800, 0x409A0048, 0x7FC3F378, 0x4803F651 };
static const uint32_t device_object_type_words[] = { 0x800644E8, 0x80064768, 0, 0, 0x8006C0F0, 0x801709D0, 0x69766544 };
static const uint32_t mass_driver_words[DRIVER_WORDS] = { 0x800E34D0, 0, 0, 0x8006B400, 0x8006B400, 0x800E1128,
	0x800E1128, 0x8006B400, 0x8006B400, 0x8008F010, 0x8006B400, 0x8006B400, 0x800E1868, 0x8006B400 };
static const uint32_t fat_driver_words[DRIVER_WORDS] = { 0, 0x800A25D0, 0x800A2650, 0x800A51C8, 0x800A4D90, 0x800A4890,
	0x800A4890, 0x800A2FA0, 0x800A3BF0, 0x800A2750, 0x800A2D18, 0x800A2A88, 0x800A2750, 0x800A28A0 };

static unsigned passed_count;
static unsigned failed_count;
static _Alignas(4096) uint8_t sector[SECTOR_BYTES_MAX];

static void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	passed_count += passed;
	failed_count += !passed;
}

// Titles run in kernel mode; the kernel's memory reads like any other.
static uint32_t kernel_word(uint32_t address)
{
	return *(volatile const uint32_t *)(uintptr_t)address;
}

static uint8_t kernel_byte(uint32_t address)
{
	return *(volatile const uint8_t *)(uintptr_t)address;
}

static bool words_match(const char *name, uint32_t address, const uint32_t *words, unsigned count)
{
	for (unsigned i = 0; i < count; i++) {
		const uint32_t word = kernel_word(address + 4 * i);
		if (word != words[i]) {
			xsdk_log_write("%s: word %u at %08X is %08X, the image has %08X", name, i, (unsigned)(address + 4 * i),
				(unsigned)word, (unsigned)words[i]);
			return false;
		}
	}
	return true;
}

static void log_words(const char *name, uint32_t address, unsigned count)
{
	for (unsigned start = 0; start < count; start += 8) {
		char line[8 * 9 + 1] = "";
		for (unsigned i = start; i < count && i < start + 8; i++)
			snprintf(line + strlen(line), sizeof(line) - strlen(line), "%08X ", (unsigned)kernel_word(address + 4 * i));
		xsdk_log_write("%s %08X: %s", name, (unsigned)(address + 4 * start), line);
	}
}

static const char *driver_name(uint32_t driver)
{
	if (driver == KERNEL_FAT_DRIVER)
		return "FAT";
	if (driver == KERNEL_HFS_DRIVER)
		return "HFS";
	if (driver == KERNEL_RAW_DRIVER)
		return "raw";
	if (driver == KERNEL_MASS_DRIVER)
		return "USB mass storage";
	return "unknown";
}

static void log_mounted(const char *when, uint32_t device)
{
	const uint32_t volume = kernel_word(device + 12);
	if (!volume) {
		xsdk_log_write("RESULT mounted %s: none", when);
		return;
	}
	const uint32_t driver = kernel_word(volume + 8);
	xsdk_log_write("RESULT mounted %s: volume %08X type %u driver %08X (%s) refs %u flags %08X", when, (unsigned)volume,
		kernel_byte(volume + 28), (unsigned)driver, driver_name(driver), (unsigned)kernel_word(volume + 4),
		(unsigned)kernel_word(volume + 20));
}

static ANSI_STRING ansi(const char *text)
{
	const ANSI_STRING string = { (uint16_t)strlen(text), (uint16_t)(strlen(text) + 1), text };
	return string;
}

static uint32_t reference_device(const char *path)
{
	const ANSI_STRING name = ansi(path);
	uint32_t object = 0;
	const NTSTATUS status = ObReferenceObjectByName(&name, 0, IoDeviceObjectType, NULL, &object);
	xsdk_log_write("%s: reference status %08X object %08X", path, (unsigned)status, (unsigned)object);
	return NT_SUCCESS(status) ? object : 0;
}

static NTSTATUS open_path(const char *path, uint32_t options, HANDLE *handle)
{
	const ANSI_STRING name = ansi(path);
	const OBJECT_ATTRIBUTES attributes = { 0, &name, 0 };
	IO_STATUS_BLOCK io = { 0, 0 };
	*handle = 0;
	const NTSTATUS status = NtCreateFile(handle, GENERIC_READ | SYNCHRONIZE, &attributes, &io, NULL, 0,
		FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, options | FILE_SYNCHRONOUS_IO_NONALERT);
	xsdk_log_write("RESULT open %s: status %08X information %u", path, (unsigned)status, (unsigned)io.information);
	return status;
}

static uint32_t le16(const uint8_t *bytes)
{
	return bytes[0] | (uint32_t)bytes[1] << 8;
}

static uint32_t le32(const uint8_t *bytes)
{
	return le16(bytes) | le16(bytes + 2) << 16;
}

static void dump_sector(const char *name)
{
	for (unsigned start = 0; start < 512; start += 16) {
		char line[16 * 3 + 1] = "";
		for (unsigned i = start; i < start + 16; i++)
			snprintf(line + strlen(line), sizeof(line) - strlen(line), "%02X ", sector[i]);
		xsdk_log_write("%s %03X: %s", name, start, line);
	}
}

static bool save_sector(const char *path, uint32_t bytes)
{
	FILE *const file = fopen(path, "wb");
	const bool written = file && fwrite(sector, 1, bytes, file) == bytes;
	if (file)
		fclose(file);
	return written;
}

// Logs what the sector in the buffer is; returns an MBR's first used partition's start, 0 otherwise.
static uint32_t identify_sector(const char *name, uint32_t sector_bytes)
{
	const bool signature = sector[510] == 0x55 && sector[511] == 0xAA;
	if (memcmp(sector + 3, "EXFAT   ", 8) == 0) {
		// The main boot sector's fields (Microsoft's exFAT specification, section 3.1), little-endian.
		const unsigned bytes_shift = sector[108];
		xsdk_log_write(
			"RESULT %s: exFAT, volume length %08X%08X sectors, FAT at %u for %u, cluster heap at %u, %u "
			"clusters, root cluster %u, serial %08X, revision %u.%u, flags %04X, %u bytes a sector, %u sectors a "
			"cluster, %u FATs",
			name, (unsigned)le32(sector + 76), (unsigned)le32(sector + 72), (unsigned)le32(sector + 80),
			(unsigned)le32(sector + 84), (unsigned)le32(sector + 88), (unsigned)le32(sector + 92),
			(unsigned)le32(sector + 96), (unsigned)le32(sector + 100), sector[105], sector[104],
			(unsigned)le16(sector + 106), 1u << bytes_shift, 1u << sector[109], sector[110]);
		check("exFAT boot sector: ends in 0x55 0xAA", signature);
		check("exFAT boot sector: its bytes a sector match the geometry", (1u << bytes_shift) == sector_bytes);
		return 0;
	}
	if (signature && (memcmp(sector + 82, "FAT32   ", 8) == 0 || memcmp(sector + 54, "FAT", 3) == 0)) {
		xsdk_log_write("RESULT %s: a FAT boot sector", name);
		return 0;
	}
	if (!signature) {
		xsdk_log_write("RESULT %s: no boot signature", name);
		return 0;
	}
	uint32_t first_start = 0;
	for (unsigned i = 0; i < 4; i++) {
		const uint8_t *const entry = sector + MBR_PARTITION_TABLE + MBR_PARTITION_ENTRY_BYTES * i;
		const uint32_t start = le32(entry + 8);
		xsdk_log_write("RESULT %s: MBR entry %u type %02X start %u sectors %u", name, i, entry[4], (unsigned)start,
			(unsigned)le32(entry + 12));
		if (entry[4] && start && !first_start)
			first_start = start;
	}
	return first_start;
}

static void read_sectors(uint32_t device, uint32_t sector_bytes)
{
	uint64_t offset = 0;
	memset(sector, 0, sizeof(sector));
	NTSTATUS status = IoSynchronousFsdRequest(IRP_MJ_READ, device, sector, sector_bytes, &offset);
	xsdk_log_write("RESULT sector0: read status %08X", (unsigned)status);
	check("sector0: read through a read IRP", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	dump_sector("sector0");
	check("sector0: sector0.bin written beside the title", save_sector("sector0.bin", sector_bytes));
	const uint32_t partition_start = identify_sector("sector0", sector_bytes);
	if (!partition_start)
		return;
	offset = (uint64_t)partition_start * sector_bytes;
	memset(sector, 0, sizeof(sector));
	status = IoSynchronousFsdRequest(IRP_MJ_READ, device, sector, sector_bytes, &offset);
	xsdk_log_write("RESULT boot: read of sector %u status %08X", (unsigned)partition_start, (unsigned)status);
	check("boot: the partition's first sector read", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	dump_sector("boot");
	check("boot: boot.bin written beside the title", save_sector("boot.bin", sector_bytes));
	(void)identify_sector("boot", sector_bytes);
}

static void log_other_slots(void)
{
	static const char *const paths[] = { "\\Device\\Mass1", "\\Device\\Mass2", "\\Device\\Joul0" };
	for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
		const uint32_t device = reference_device(paths[i]);
		if (!device)
			continue;
		xsdk_log_write("%s: device type %u flags %08X driver %08X (%s)", paths[i], kernel_byte(device + 28),
			(unsigned)kernel_word(device + 20), (unsigned)kernel_word(device + 8),
			driver_name(kernel_word(device + 8)));
		log_mounted(paths[i], device);
		ObDereferenceObject(device);
	}
}

int main(void)
{
	xsdk_log_write("probe_exfat_seam: kernel %u.%u.%u.%u", XboxKrnlVersion->major, XboxKrnlVersion->minor,
		XboxKrnlVersion->build, XboxKrnlVersion->qfe);
	if (XboxKrnlVersion->build != KERNEL_BUILD) {
		xsdk_log_write("probe_exfat_seam: the addresses are kernel 17559's; nothing runs");
		return 1;
	}
	const NTSTATUS splash = xsdk_splash_show(SPLASH_MS);
	check("splash: the vulkan4xenon logo shown", NT_SUCCESS(splash));

	check("kernel: the boot hook's words match the image",
		words_match("boot hook", KERNEL_BOOT_HOOK, boot_hook_words, sizeof(boot_hook_words) / 4));
	check("kernel: the mount chain's words match the image",
		words_match("mount chain", KERNEL_MOUNT_CHAIN, mount_chain_words, sizeof(mount_chain_words) / 4));
	check("kernel: IoDeviceObjectType matches the image",
		words_match("IoDeviceObjectType", KERNEL_IO_DEVICE_OBJECT_TYPE, device_object_type_words,
			sizeof(device_object_type_words) / 4));
	check("kernel: the IoDeviceObjectType import is 0x80040B08",
		(uint32_t)(uintptr_t)IoDeviceObjectType == KERNEL_IO_DEVICE_OBJECT_TYPE);
	check("kernel: FAT's driver object matches the image",
		words_match("FAT driver", KERNEL_FAT_DRIVER, fat_driver_words, DRIVER_WORDS));
	check("kernel: the .text padding is empty",
		kernel_word(KERNEL_TEXT_PADDING) == 0 && kernel_word(KERNEL_TEXT_PADDING_END) == 0);
	xsdk_log_write("RESULT protect: padding %08X mount chain %08X .rdata %08X .data %08X",
		(unsigned)MmQueryAddressProtect(KERNEL_TEXT_PADDING), (unsigned)MmQueryAddressProtect(KERNEL_MOUNT_CHAIN),
		(unsigned)MmQueryAddressProtect(KERNEL_IO_DEVICE_OBJECT_TYPE),
		(unsigned)MmQueryAddressProtect(KERNEL_IO_SPIN_LOCK));

	const uint32_t device = reference_device("\\Device\\Mass0");
	check("mass0: referenced by name", device != 0);
	if (device) {
		log_words("mass0 device object", device, DEVICE_OBJECT_BYTES / 4);
		const uint32_t driver = kernel_word(device + 8);
		xsdk_log_write(
			"mass0: type %u size %u refs %u driver %08X (%s) flags %08X extension %08X device type %u stack %u "
			"sector size %u alignment %u mount lock type %u state %u",
			kernel_word(device) >> 16, kernel_word(device) & 0xFFFF, (unsigned)kernel_word(device + 4),
			(unsigned)driver, driver_name(driver), (unsigned)kernel_word(device + 20),
			(unsigned)kernel_word(device + 24), kernel_byte(device + 28), kernel_byte(device + 30),
			(unsigned)kernel_word(device + 32), (unsigned)kernel_word(device + 36), kernel_byte(device + 56),
			(unsigned)kernel_word(device + 60));
		check("mass0: device object type 3", kernel_word(device) >> 16 == 3);
		check("mass0: device type 45", kernel_byte(device + 28) == MASS_DEVICE_TYPE);
		check("mass0: driven by the USB mass storage class", driver == KERNEL_MASS_DRIVER);
		check("mass0: the extension follows the 80-byte object",
			kernel_word(device + 24) == device + DEVICE_OBJECT_BYTES);
		check("mass0: the mount lock is signaled", kernel_word(device + 60) == 1);
		log_words("mass0 driver object", driver, DRIVER_WORDS);
		check("mass0: the driver table matches the image",
			words_match("Mass driver", driver, mass_driver_words, DRIVER_WORDS));
		log_mounted("before any open", device);

		uint32_t geometry[2] = { 0, 0 };
		uint32_t returned = 0;
		NTSTATUS status = IoSynchronousDeviceIoControlRequest(
			IOCTL_DISK_GET_DRIVE_GEOMETRY, device, NULL, 0, geometry, sizeof(geometry), &returned);
		const uint32_t sector_bytes = geometry[1];
		xsdk_log_write("RESULT geometry: status %08X returned %u sectors %u bytes a sector %u (%u MB)",
			(unsigned)status, (unsigned)returned, (unsigned)geometry[0], (unsigned)sector_bytes,
			(unsigned)(((uint64_t)geometry[0] * sector_bytes) >> 20));
		const bool media = NT_SUCCESS(status) && sector_bytes >= 512 && sector_bytes <= SECTOR_BYTES_MAX &&
			(sector_bytes & (sector_bytes - 1)) == 0;
		check("mass0: the geometry answers with a stick in", media);
		uint32_t partition[4] = { 0, 0, 0, 0 };
		status = IoSynchronousDeviceIoControlRequest(
			IOCTL_DISK_GET_PARTITION_INFO, device, NULL, 0, partition, sizeof(partition), &returned);
		xsdk_log_write("RESULT partition: status %08X returned %u start %08X%08X length %08X%08X (%u MB)",
			(unsigned)status, (unsigned)returned, (unsigned)partition[0], (unsigned)partition[1],
			(unsigned)partition[2], (unsigned)partition[3],
			(unsigned)((((uint64_t)partition[2] << 32) | partition[3]) >> 20));
		if (media)
			read_sectors(device, sector_bytes);

		HANDLE handle;
		if (NT_SUCCESS(open_path("\\Device\\Mass0\\", FILE_DIRECTORY_FILE, &handle)))
			NtClose(handle);
		log_mounted("after the root open", device);
		HANDLE device_handle = 0;
		if (NT_SUCCESS(open_path("\\Device\\Mass0", 0, &handle)))
			device_handle = handle;
		log_mounted("after the device open", device);
		if (NT_SUCCESS(open_path("\\Device\\Mass0\\", FILE_DIRECTORY_FILE, &handle)))
			NtClose(handle);
		if (device_handle)
			NtClose(device_handle);
		log_mounted("after closing", device);
		ObDereferenceObject(device);
	}
	log_other_slots();
	xsdk_log_write("probe_exfat_seam: %u passed, %u failed", passed_count, failed_count);
	return failed_count != 0;
}
