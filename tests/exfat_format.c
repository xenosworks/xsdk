#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xsdk/exfat.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/splash.h"
#include "xsdk/text.h"
#include "xsdk/thread.h"

// Milestone 77 step 5 and milestone 78 step 2 (docs/scope/77-exfat.md, docs/scope/78-exfat-hdd.md): formats a USB
// stick, or the drive's content partition, exFAT from the console. The title lists the Mass devices holding media and
// \Device\Harddisk0\Partition1, each with its size and what its first sector holds; the pad picks one, and A held for
// three seconds formats it, the drive after a second hold on a screen of its own. exfat_format.txt beside the title,
// "device label", formats that device with no pad, for console-agent runs; with neither a pad nor the file the title
// lists for a few seconds and exits. The format is step 2's (xsdk_exfat_format) through read and write IRPs to the
// device object, as step 1's writer wrote the stick, once IoDismountVolumeByName has taken the kernel's volume off it;
// the result is read back by mounting the volume, or by the module's mount of it. The device's mount lock is held
// through the format, as the kernel holds it to mount (plugins/exfat/kernel_seam.md, section 3), so neither the kernel
// nor the module mounts what is half written. Kernel 17559 only, as every title that writes a device raw.

enum {
	KERNEL_BUILD = 17559,
	SPLASH_MS = 1000,
	BUFFERS = 3,
	SWAP_INTERVAL = 1,
	BEGIN_TIMEOUT_MS = 5000,
	PAD_WAIT_MS = 1000,
	PAD_POLL_MS = 50,
	LIST_ONLY_MS = 5000,
	HOLD_MS = 3000,
	// Three of the module's 500 ms polls, for its mount of a formatted stick.
	MODULE_WAIT_MS = 1500,
	DISMOUNT_TRIES = 3,
	FILETIME_PER_MS = 10000,
	SCALE = 2,
	MARGIN_X = 80,
	TOP_Y = 60,
	LINE_HEIGHT = XSDK_TEXT_CELL_HEIGHT * SCALE + 8,
	TEXT_MAX = 128,
	PATH_BYTES = 64,
	SECTOR_BYTES_MAX = EXFAT_SECTOR_BYTES_MAX,
	CHUNK_BYTES = 65536,
	IRP_MJ_READ = 2,
	IRP_MJ_WRITE = 3,
	IOCTL_DISK_GET_DRIVE_GEOMETRY = 0x70000,
	IOCTL_DISK_GET_PARTITION_INFO = 0x74004,
	// The device object's mounted volume, type byte and mount lock (plugins/exfat/kernel_io.h, DEVICE_OBJECT).
	MOUNTED_OFFSET = 0x0C,
	DEVICE_TYPE_OFFSET = 0x1C,
	MOUNT_LOCK_OFFSET = 0x38,
	MBR_PARTITION_TABLE = 446,
	MBR_TYPE_EXFAT = 0x07,
	DEVICES_MAX = 4,
	BACKGROUND_ARGB = 0xFF101820,
	TEXT_ARGB = 0xFFE0E0E0,
	DIM_ARGB = 0xFF8090A0,
	WARN_ARGB = 0xFFFFB040,
	OK_ARGB = 0xFF60E060,
	BAD_ARGB = 0xFFFF6060,
};
static const char FILE_PATH[] = "exfat_format.txt";

typedef struct {
	const char *path;
	const char *title;
	bool drive;
	// The device object's address, 0 where the device is absent or holds no media.
	uint32_t object;
	uint32_t device_type;
	uint32_t sector_bytes;
	uint64_t sectors;
	char holds[48];
} Device;

static Device devices[DEVICES_MAX] = {
	{ "\\Device\\Mass0", "USB stick 0", false, 0, 0, 0, 0, "" },
	{ "\\Device\\Mass1", "USB stick 1", false, 0, 0, 0, 0, "" },
	{ "\\Device\\Mass2", "USB stick 2", false, 0, 0, 0, 0, "" },
	{ "\\Device\\Harddisk0\\Partition1", "HDD content partition", true, 0, 0, 0, 0, "" },
};

typedef enum { LISTING, CONFIRM_DRIVE, DONE } Screen;

static uint8_t *bounce;
static _Alignas(4096) uint8_t sector[SECTOR_BYTES_MAX];
static ExfatVolume volume;
// Off once the partition the log lives on is dismounted.
static bool log_open = true;
static uint32_t width;
static uint32_t height;

static void say(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *format, ...)
{
	if (!log_open)
		return;
	char text[TEXT_MAX * 2];
	va_list args;
	va_start(args, format);
	(void)vsnprintf(text, sizeof text, format, args);
	va_end(args);
	xsdk_log_write("%s", text);
}

static ANSI_STRING ansi(const char *text)
{
	const ANSI_STRING string = { (uint16_t)strlen(text), (uint16_t)(strlen(text) + 1), text };
	return string;
}

static uint64_t now_filetime(void)
{
	uint64_t time;
	KeQuerySystemTime(&time);
	return time;
}

// Sectors from first through the bounce buffer, a chunk at a time; true when every IRP succeeded.
static bool transfer(const Device *d, uint32_t major, uint64_t first, void *buffer, uint32_t bytes)
{
	uint8_t *at = buffer;
	uint64_t offset = first * d->sector_bytes;
	while (bytes) {
		const uint32_t chunk = bytes < CHUNK_BYTES ? bytes : CHUNK_BYTES;
		if (major == IRP_MJ_WRITE)
			memcpy(bounce, at, chunk);
		const NTSTATUS status = IoSynchronousFsdRequest(major, d->object, bounce, chunk, &offset);
		if (!NT_SUCCESS(status)) {
			say("%s: %s of %u bytes at %llu failed: 0x%08X", d->path, major == IRP_MJ_READ ? "read" : "write", chunk,
				(unsigned long long)offset, (unsigned)status);
			return false;
		}
		if (major == IRP_MJ_READ)
			memcpy(at, bounce, chunk);
		at += chunk;
		offset += chunk;
		bytes -= chunk;
	}
	return true;
}

static int device_read(void *context, uint64_t sector, uint32_t count, void *buffer)
{
	const Device *const d = context;
	return transfer(d, IRP_MJ_READ, sector, buffer, count * d->sector_bytes) ? 0 : -1;
}

static int device_write(void *context, uint64_t sector, uint32_t count, const void *buffer)
{
	const Device *const d = context;
	return transfer(d, IRP_MJ_WRITE, sector, (void *)buffer, count * d->sector_bytes) ? 0 : -1;
}

static uint64_t device_now(void *context)
{
	(void)context;
	return now_filetime();
}

static bool all_zero(const uint8_t *bytes, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		if (bytes[i])
			return false;
	}
	return true;
}

// What the device's first sector holds, into d->holds.
static void describe(Device *d)
{
	if (!transfer(d, IRP_MJ_READ, 0, sector, d->sector_bytes)) {
		strcpy(d->holds, "unreadable");
		return;
	}
	const uint8_t *const entry = sector + MBR_PARTITION_TABLE;
	const bool signed_sector = sector[510] == 0x55 && sector[511] == 0xAA;
	if (memcmp(sector + 3, "EXFAT   ", 8) == 0)
		strcpy(d->holds, "exFAT");
	else if (memcmp(sector, "XTAF", 4) == 0)
		strcpy(d->holds, "FATX");
	else if (signed_sector && (memcmp(sector + 82, "FAT32   ", 8) == 0 || memcmp(sector + 54, "FAT1", 4) == 0))
		strcpy(d->holds, "FAT");
	else if (signed_sector && entry[4] == MBR_TYPE_EXFAT) {
		const uint32_t start =
			entry[8] | (uint32_t)entry[9] << 8 | (uint32_t)entry[10] << 16 | (uint32_t)entry[11] << 24;
		const bool exfat =
			transfer(d, IRP_MJ_READ, start, sector, d->sector_bytes) && memcmp(sector + 3, "EXFAT   ", 8) == 0;
		strcpy(d->holds, exfat ? "exFAT (MBR)" : "MBR, partition type 07");
	} else if (signed_sector && entry[4])
		(void)snprintf(d->holds, sizeof d->holds, "MBR, partition type %02X", entry[4]);
	else
		strcpy(d->holds, all_zero(sector, d->sector_bytes) ? "nothing" : "unknown");
}

// References the device and reads its geometry; false where it is absent or holds no media (the geometry IOCTL
// answers only with media in; the partition IOCTL gives the length in 64 bits, as the module reads them).
static bool probe(Device *d)
{
	const ANSI_STRING name = ansi(d->path);
	uint32_t object = 0;
	if (!NT_SUCCESS(ObReferenceObjectByName(&name, 0, IoDeviceObjectType, NULL, &object)) || !object)
		return false;
	uint32_t geometry[2] = { 0, 0 };
	const NTSTATUS status = IoSynchronousDeviceIoControlRequest(
		IOCTL_DISK_GET_DRIVE_GEOMETRY, object, NULL, 0, geometry, sizeof geometry, NULL);
	if (!NT_SUCCESS(status) || geometry[1] < 512 || geometry[1] > SECTOR_BYTES_MAX ||
		(geometry[1] & (geometry[1] - 1))) {
		say("%s: present, no media (geometry 0x%08X)", d->path, (unsigned)status);
		(void)ObDereferenceObject(object);
		return false;
	}
	d->object = object;
	d->device_type = *(volatile const uint8_t *)(uintptr_t)(object + DEVICE_TYPE_OFFSET);
	d->sector_bytes = geometry[1];
	d->sectors = geometry[0];
	uint32_t partition[4] = { 0, 0, 0, 0 };
	if (NT_SUCCESS(IoSynchronousDeviceIoControlRequest(
			IOCTL_DISK_GET_PARTITION_INFO, object, NULL, 0, partition, sizeof partition, NULL)) &&
		(partition[2] | partition[3]))
		d->sectors = ((uint64_t)partition[2] << 32 | partition[3]) / d->sector_bytes;
	describe(d);
	const uint64_t tenths = d->sectors * d->sector_bytes / 100000000;
	say("%s (%s): device type %u, %llu sectors of %u, %llu.%llu GB, holds %s", d->path, d->title, d->device_type,
		(unsigned long long)d->sectors, d->sector_bytes, (unsigned long long)(tenths / 10),
		(unsigned long long)(tenths % 10), d->holds);
	return true;
}

static uint32_t mounted_volume(const Device *d)
{
	return *(volatile const uint32_t *)(uintptr_t)(d->object + MOUNTED_OFFSET);
}

// The kernel's volume off the device and the mount lock held with nothing mounted; false when a volume stays.
static bool take_device(Device *d, char *report, size_t report_bytes)
{
	const ANSI_STRING name = ansi(d->path);
	void *const lock = (void *)(uintptr_t)(d->object + MOUNT_LOCK_OFFSET);
	for (unsigned try = 0; try < DISMOUNT_TRIES; try++) {
		const NTSTATUS dismounted = IoDismountVolumeByName(&name);
		say("%s: dismount 0x%08X", d->path, (unsigned)dismounted);
		(void)KeWaitForSingleObject(lock, 0, KERNEL_MODE, 0, NULL);
		if (!mounted_volume(d))
			return true;
		(void)KeSetEvent(lock, 0, 0);
	}
	(void)snprintf(report, report_bytes, "A volume stays mounted on the device; nothing written");
	say("%s: %s", d->path, report);
	return false;
}

// Dismounts the kernel's volume, formats under the mount lock, then reads the result back: the module's mount of it,
// or this title's own; the report for the screen.
static bool format(Device *d, const char *label, char *report, size_t report_bytes)
{
	say("%s: formatting exFAT, label \"%s\"", d->path, label);
	// The log lives beside the title: closed before the dismount when the title runs from the partition it formats.
	if (d->drive && ExLoadedImageName && strncmp(ExLoadedImageName, d->path, strlen(d->path)) == 0) {
		xsdk_log_close();
		log_open = false;
	}
	if (!take_device(d, report, report_bytes))
		return false;
	void *const lock = (void *)(uintptr_t)(d->object + MOUNT_LOCK_OFFSET);
	ExfatDevice device = { device_read, device_write, device_now, d, d->sector_bytes, 0, d->sectors };
	ExfatStatus status = xsdk_exfat_format(&device, label, (uint32_t)now_filetime());
	(void)KeSetEvent(lock, 0, 0);
	if (status != EXFAT_OK) {
		(void)snprintf(report, report_bytes, "Format failed: %s", xsdk_exfat_status_name(status));
		say("%s: %s", d->path, report);
		return false;
	}
	for (uint32_t waited = 0; waited < MODULE_WAIT_MS && !mounted_volume(d); waited += PAD_POLL_MS)
		(void)xsdk_thread_sleep(PAD_POLL_MS);
	if (mounted_volume(d)) {
		const uint64_t tenths = d->sectors * d->sector_bytes / 100000000;
		(void)snprintf(report, report_bytes, "Formatted exFAT, %llu.%llu GB; the module mounted it",
			(unsigned long long)(tenths / 10), (unsigned long long)(tenths % 10));
	} else {
		status = xsdk_exfat_locate(&device);
		if (status == EXFAT_OK)
			status = xsdk_exfat_mount(&volume, &device);
		if (status != EXFAT_OK) {
			(void)snprintf(
				report, report_bytes, "Formatted, but the volume does not mount: %s", xsdk_exfat_status_name(status));
			say("%s: %s", d->path, report);
			return false;
		}
		(void)snprintf(report, report_bytes, "Formatted exFAT: %u clusters of %u KB, %u free",
			(unsigned)volume.cluster_count, (unsigned)(1u << volume.cluster_shift >> 10),
			(unsigned)volume.free_clusters);
		(void)xsdk_exfat_unmount(&volume);
	}
	say("%s: %s", d->path, report);
	describe(d);
	return true;
}

static Device *device_at_path(const char *path)
{
	for (uint32_t i = 0; i < DEVICES_MAX; i++) {
		if (strcmp(devices[i].path, path) == 0)
			return &devices[i];
	}
	return NULL;
}

// exfat_format.txt's device and label; false without the file.
static bool read_request(char path[PATH_BYTES], char label[EXFAT_LABEL_MAX + 1])
{
	FILE *const file = fopen(FILE_PATH, "r");
	if (!file)
		return false;
	path[0] = 0;
	label[0] = 0;
	(void)fscanf(file, "%63s %11s", path, label);
	fclose(file);
	return true;
}

static void draw(int32_t y, uint32_t argb, const char *format, ...) __attribute__((format(printf, 3, 4)));
static void draw(int32_t y, uint32_t argb, const char *format, ...)
{
	char text[TEXT_MAX];
	va_list args;
	va_start(args, format);
	(void)vsnprintf(text, sizeof text, format, args);
	va_end(args);
	xsdk_text_draw(MARGIN_X, y, SCALE, argb, text);
}

static void draw_list(uint32_t selected, uint32_t held_ms)
{
	draw(TOP_Y, TEXT_ARGB, "exFAT format");
	draw(TOP_Y + LINE_HEIGHT, DIM_ARGB, "Up and down pick a device. Hold A for 3 s to format it exFAT. Back exits.");
	int32_t y = TOP_Y + 3 * LINE_HEIGHT;
	bool any = false;
	for (uint32_t i = 0; i < DEVICES_MAX; i++) {
		const Device *const d = &devices[i];
		if (!d->object)
			continue;
		any = true;
		const uint64_t tenths = d->sectors * d->sector_bytes / 100000000;
		draw(y, i == selected ? TEXT_ARGB : DIM_ARGB, "%c %-22s %6llu.%llu GB   %s", i == selected ? '>' : ' ',
			d->title, (unsigned long long)(tenths / 10), (unsigned long long)(tenths % 10), d->holds);
		y += LINE_HEIGHT;
	}
	if (!any)
		draw(y, WARN_ARGB, "No stick holds media.");
	if (held_ms)
		draw(y + LINE_HEIGHT, WARN_ARGB, "Formatting %s in %u.%u s, release A to stop", devices[selected].title,
			(unsigned)((HOLD_MS - held_ms) / 1000), (unsigned)((HOLD_MS - held_ms) % 1000 / 100));
}

static void draw_confirm(const Device *d, uint32_t held_ms)
{
	draw(TOP_Y, WARN_ARGB, "Format the %s?", d->title);
	draw(TOP_Y + 2 * LINE_HEIGHT, TEXT_ARGB, "Everything on the content partition is erased: Aurora, content, saves.");
	draw(TOP_Y + 3 * LINE_HEIGHT, TEXT_ARGB, "The system partitions stay FATX. The dash's own Format takes it back.");
	draw(TOP_Y + 5 * LINE_HEIGHT, DIM_ARGB, "Hold A for 3 s again to format. B goes back.");
	if (held_ms)
		draw(TOP_Y + 7 * LINE_HEIGHT, WARN_ARGB, "Formatting in %u.%u s, release A to stop",
			(unsigned)((HOLD_MS - held_ms) / 1000), (unsigned)((HOLD_MS - held_ms) % 1000 / 100));
}

static void draw_done(const Device *d, const char *report, bool formatted)
{
	draw(TOP_Y, TEXT_ARGB, "%s", d->title);
	draw(TOP_Y + 2 * LINE_HEIGHT, formatted ? OK_ARGB : BAD_ARGB, "%s", report);
	if (formatted && d->drive) {
		draw(TOP_Y + 4 * LINE_HEIGHT, WARN_ARGB,
			"The drive has no dash now. Reboot with a USB stick holding Aurora and a");
		draw(TOP_Y + 5 * LINE_HEIGHT, WARN_ARGB,
			"launch.ini: DashLaunch reads the stick first, and Aurora copies itself back.");
	} else if (formatted)
		draw(TOP_Y + 4 * LINE_HEIGHT, DIM_ARGB, "The module mounts it within a second. Back exits.");
	else
		draw(TOP_Y + 4 * LINE_HEIGHT, DIM_ARGB, "Back exits.");
}

// One frame: what the screen shows, resolved into the front buffer.
static void show(
	Screen screen, uint32_t selected, uint32_t held_ms, const char *report, bool formatted, uint32_t address)
{
	xsdk_gpu_set_target(width, height);
	switch (screen) {
	case LISTING:
		draw_list(selected, held_ms);
		break;
	case CONFIRM_DRIVE:
		draw_confirm(&devices[selected], held_ms);
		break;
	case DONE:
		draw_done(&devices[selected], report, formatted);
		break;
	}
	xsdk_gpu_resolve(address, width, height, BACKGROUND_ARGB);
}

static void show_formatting(const Device *d, uint32_t address)
{
	xsdk_gpu_set_target(width, height);
	draw(TOP_Y, TEXT_ARGB, "Formatting the %s...", d->title);
	xsdk_gpu_resolve(address, width, height, BACKGROUND_ARGB);
}

static uint32_t next_selectable(uint32_t from, int step)
{
	for (uint32_t i = 1; i <= DEVICES_MAX; i++) {
		const uint32_t at = (from + DEVICES_MAX + (uint32_t)(step * (int)i)) % DEVICES_MAX;
		if (devices[at].object)
			return at;
	}
	return from;
}

static bool pad_present(void)
{
	INPUT_STATE state;
	for (uint32_t waited = 0; waited < PAD_WAIT_MS; waited += PAD_POLL_MS) {
		if (XamInputGetState(0, 0, &state) == ERROR_SUCCESS)
			return true;
		(void)xsdk_thread_sleep(PAD_POLL_MS);
	}
	return false;
}

// The list, the picks and the format, until Back; returns the exit status.
static int run_ui(bool interactive)
{
	Screen screen = LISTING;
	uint32_t selected = next_selectable(DEVICES_MAX - 1, 1);
	uint32_t held_ms = 0;
	uint64_t hold_started = 0;
	uint16_t was_down = 0;
	char report[TEXT_MAX] = "";
	bool formatted = false;
	const uint64_t shown_at = now_filetime();
	for (;;) {
		uint32_t address;
		if (!NT_SUCCESS(xsdk_present_begin(&address, BEGIN_TIMEOUT_MS))) {
			say("exfat_format: no front buffer in %u ms", BEGIN_TIMEOUT_MS);
			return 1;
		}
		if (!interactive) {
			show(LISTING, selected, 0, "", false, address);
			xsdk_present_end();
			if (now_filetime() - shown_at > (uint64_t)LIST_ONLY_MS * FILETIME_PER_MS)
				return 0;
			continue;
		}
		INPUT_STATE state;
		const uint16_t down = XamInputGetState(0, 0, &state) == ERROR_SUCCESS ? state.gamepad.buttons : 0;
		const uint16_t pressed = down & ~was_down;
		was_down = down;
		if (pressed & INPUT_GAMEPAD_BACK) {
			show(screen, selected, 0, report, formatted, address);
			xsdk_present_end();
			return 0;
		}
		if (screen == LISTING && (pressed & INPUT_GAMEPAD_DPAD_UP))
			selected = next_selectable(selected, -1);
		if (screen == LISTING && (pressed & INPUT_GAMEPAD_DPAD_DOWN))
			selected = next_selectable(selected, 1);
		if (screen == CONFIRM_DRIVE && (pressed & INPUT_GAMEPAD_B))
			screen = LISTING;
		const bool can_hold = screen != DONE && devices[selected].object;
		if (can_hold && (down & INPUT_GAMEPAD_A)) {
			if (!hold_started)
				hold_started = now_filetime();
			held_ms = (uint32_t)((now_filetime() - hold_started) / FILETIME_PER_MS);
		} else {
			hold_started = 0;
			held_ms = 0;
		}
		if (held_ms >= HOLD_MS) {
			hold_started = 0;
			held_ms = 0;
			if (screen == LISTING && devices[selected].drive)
				screen = CONFIRM_DRIVE;
			else {
				show_formatting(&devices[selected], address);
				xsdk_present_end();
				formatted = format(&devices[selected], "", report, sizeof report);
				screen = DONE;
				continue;
			}
		}
		show(screen, selected, held_ms, report, formatted, address);
		xsdk_present_end();
	}
}

int main(void)
{
	(void)xsdk_splash_show(SPLASH_MS);
	say("exfat_format: kernel %u.%u.%u.%u", XboxKrnlVersion->major, XboxKrnlVersion->minor, XboxKrnlVersion->build,
		XboxKrnlVersion->qfe);
	if (XboxKrnlVersion->build != KERNEL_BUILD) {
		say("exfat_format: the device layouts are kernel %u's; not touching any device", KERNEL_BUILD);
		return 1;
	}
	bounce = MmAllocatePhysicalMemoryEx(0, CHUNK_BYTES, PAGE_READWRITE, 0, UINT32_MAX, 0);
	if (!bounce) {
		say("exfat_format: no memory for the transfer buffer");
		return 1;
	}
	for (uint32_t i = 0; i < DEVICES_MAX; i++)
		(void)probe(&devices[i]);

	char path[PATH_BYTES];
	char label[EXFAT_LABEL_MAX + 1];
	int result = 0;
	if (read_request(path, label)) {
		Device *const d = device_at_path(path);
		if (!d || !d->object) {
			say("exfat_format: %s names \"%s\", which is not a device holding media; nothing formatted", FILE_PATH,
				path);
			result = 1;
		} else {
			char report[TEXT_MAX];
			result = format(d, label, report, sizeof report) ? 0 : 1;
		}
	} else {
		const bool interactive = pad_present();
		if (!interactive)
			say("exfat_format: no pad and no %s; listing only", FILE_PATH);
		const bool opened = NT_SUCCESS(xsdk_gpu_open()) && NT_SUCCESS(xsdk_text_open()) &&
			NT_SUCCESS(xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &width, &height));
		if (!opened) {
			say("exfat_format: the GPU, the text or the display did not open");
			result = 1;
		} else {
			result = run_ui(interactive);
			xsdk_present_close();
			xsdk_text_close();
			(void)xsdk_gpu_close();
		}
	}
	for (uint32_t i = 0; i < DEVICES_MAX; i++) {
		if (devices[i].object)
			(void)ObDereferenceObject(devices[i].object);
	}
	return result;
}
