#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"

// Milestone 54 step 2's probe (docs/scope/54-porters-files-and-system.md): which device paths the console has for the
// hard disk, USB drives, memory units and the DVD. For each, its root opened as a folder with its closing backslash and
// without, up to 8 names listed from it, and statvfs of it. Its log aside, it only reads: it opens with FILE_OPEN and
// GENERIC_READ, writes, deletes and formats nothing, and never names the flash. Run it with and without a USB stick and
// a disc in.

enum {
	MAX_NAMES = 8,
	PATH_BYTES = 64,
	NAMES_BYTES = 512,
	QUERY_BYTES = 1024,
};

// Xenia's (xenia:src/xenia/emulator.cc:298-353), xsdk's log's fallback (src/log.c), and the others freestyledash mounts
// for USB, memory units and the DVD (github.com/XboxUnity/freestyledash, Freestyle/Tools/Managers/Drives/
// DrivesManager.cpp:15-32) or RB3Enhanced names (github.com/RBEnhanced/RB3Enhanced, source/xbox360_files.c:18-25).
static const char *const devices[] = {
	"\\Device\\Harddisk0\\Partition1",
	"\\Device\\Harddisk0\\Partition0",
	"\\Device\\Mass0",
	"\\Device\\Mass1",
	"\\Device\\Mass2",
	"\\Device\\Mass0PartitionFile\\Storage",
	"\\Device\\Mass1PartitionFile\\Storage",
	"\\Device\\Mass2PartitionFile\\Storage",
	"\\Device\\Mu0",
	"\\Device\\Mu1",
	"\\Device\\BuiltInMuSfc",
	"\\Device\\BuiltInMuUsb\\Storage",
	"\\Device\\BuiltInMuMmc\\Storage",
	"\\Device\\Cdrom0",
};

static NTSTATUS open_folder(const char *path, HANDLE *handle)
{
	const ANSI_STRING name = {
		.length = (uint16_t)strlen(path), .maximum_length = (uint16_t)(strlen(path) + 1), .pointer = path
	};
	const OBJECT_ATTRIBUTES attributes = { .root_directory = 0, .name = &name };
	IO_STATUS_BLOCK io_status;
	return NtCreateFile(handle, GENERIC_READ | SYNCHRONIZE, &attributes, &io_status, NULL, FILE_ATTRIBUTE_NORMAL,
		FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_DIRECTORY_FILE);
}

// Appends the names one query gave, up to MAX_NAMES in all, to names; count is how many it holds.
static void append_names(const uint64_t *buffer, char *names, size_t names_bytes, int *count)
{
	for (uint32_t offset = 0; offset + sizeof(FILE_DIRECTORY_INFORMATION) <= QUERY_BYTES && *count < MAX_NAMES;) {
		const FILE_DIRECTORY_INFORMATION *entry = (const FILE_DIRECTORY_INFORMATION *)((const char *)buffer + offset);
		const uint32_t room = QUERY_BYTES - offset - sizeof(*entry);
		const int length = (int)(entry->file_name_length < room ? entry->file_name_length : room);
		const size_t used = strlen(names);
		(void)snprintf(names + used, names_bytes - used, "%s%.*s%s", *count ? "," : "", length, entry->file_name,
			entry->attributes & FILE_ATTRIBUTE_DIRECTORY ? "/" : "");
		(*count)++;
		if (entry->next_entry_offset == 0)
			return;
		offset += entry->next_entry_offset;
	}
}

// Up to MAX_NAMES names in the open folder, a folder's ending in '/'; returns the last query's status.
static NTSTATUS list_names(HANDLE handle, char *names, size_t names_bytes, int *count)
{
	static uint64_t buffer[QUERY_BYTES / sizeof(uint64_t)];
	static const ANSI_STRING all = { .length = 1, .maximum_length = 2, .pointer = "*" };
	names[0] = '\0';
	*count = 0;
	NTSTATUS status = STATUS_SUCCESS;
	for (bool first = true; *count < MAX_NAMES; first = false) {
		// A FAT32 stick's driver leaves next_entry_offset as it finds it (docs/HARDWARE.md), so 0 ends the entries.
		memset(buffer, 0, sizeof(buffer));
		IO_STATUS_BLOCK io_status;
		status = NtQueryDirectoryFile(handle, 0, NULL, NULL, &io_status, (FILE_DIRECTORY_INFORMATION *)buffer,
			sizeof(buffer), first ? &all : NULL, first);
		if (!NT_SUCCESS(status))
			break;
		append_names(buffer, names, names_bytes, count);
	}
	return status;
}

static void probe_device(const char *device)
{
	char root[PATH_BYTES];
	(void)snprintf(root, sizeof(root), "%s\\", device);
	HANDLE root_handle = 0;
	HANDLE bare_handle = 0;
	const NTSTATUS root_status = open_folder(root, &root_handle);
	const NTSTATUS bare_status = open_folder(device, &bare_handle);
	char names[NAMES_BYTES] = "";
	int count = 0;
	NTSTATUS list_status = STATUS_UNSUCCESSFUL;
	if (NT_SUCCESS(root_status) || NT_SUCCESS(bare_status))
		list_status = list_names(NT_SUCCESS(root_status) ? root_handle : bare_handle, names, sizeof(names), &count);
	if (NT_SUCCESS(root_status))
		(void)NtClose(root_handle);
	if (NT_SUCCESS(bare_status))
		(void)NtClose(bare_handle);
	if (NT_SUCCESS(root_status) || NT_SUCCESS(bare_status)) {
		xsdk_log_write(
			"drives: %s: opened, %d entries%s: %s (listing ended 0x%08X); as a folder 0x%08X with its "
			"backslash, 0x%08X without",
			device, count, count == MAX_NAMES ? " or more" : "", names, (unsigned)list_status, (unsigned)root_status,
			(unsigned)bare_status);
	} else {
		xsdk_log_write("drives: %s: not opened, 0x%08X with its backslash, 0x%08X without", device,
			(unsigned)root_status, (unsigned)bare_status);
	}

	struct statvfs info;
	if (statvfs(device, &info) == 0) {
		xsdk_log_write("drives: %s: statvfs: %lu-byte units, %llu of %llu free", device, info.f_bsize,
			(unsigned long long)info.f_bfree, (unsigned long long)info.f_blocks);
	} else {
		xsdk_log_write("drives: %s: statvfs failed, errno %d", device, errno);
	}
}

int main(void)
{
	const size_t device_count = sizeof(devices) / sizeof(devices[0]);
	xsdk_log_write("drives: %u device paths", (unsigned)device_count);
	for (size_t i = 0; i < device_count; i++)
		probe_device(devices[i]);
	xsdk_log_write("drives: done");
	return 0;
}
