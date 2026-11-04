#include <stdint.h>
#include <string.h>

#include "probe.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

// Milestone 70's probe: which attributes the console's FATX keeps. chmod sets the read-only attribute through
// NtSetInformationFile's basic class, and xsdk_test read it back unset, the file still opening to write, though the
// same call sets the times. For a file and a folder in game:, each attribute value is set through an open handle and
// read back through it and by name; then a file is made read-only by NtCreateFile and opened to write.

enum {
	FILE_ATTRIBUTE_HIDDEN = 0x00000002,
	FILE_ATTRIBUTE_SYSTEM = 0x00000004,
	FILE_ATTRIBUTE_ARCHIVE = 0x00000020,
	// A time set alongside: 2001-09-09, as xsdk_test's utime sets.
	SOME_NT_TIME = 116444736000000000ull + 1000000000ull * 10000000ull,
};

static OBJECT_ATTRIBUTES named(ANSI_STRING *name, const char *path)
{
	*name = (ANSI_STRING){
		.length = (uint16_t)strlen(path), .maximum_length = (uint16_t)(strlen(path) + 1), .pointer = path
	};
	return (OBJECT_ATTRIBUTES){ .root_directory = OB_DOS_DEVICES, .name = name };
}

static NTSTATUS open_path(
	HANDLE *handle, const char *path, uint32_t access, uint32_t attributes, uint32_t disposition, uint32_t options)
{
	ANSI_STRING name;
	const OBJECT_ATTRIBUTES object = named(&name, path);
	IO_STATUS_BLOCK io_status;
	return NtCreateFile(handle, access | SYNCHRONIZE, &object, &io_status, NULL, attributes,
		FILE_SHARE_READ | FILE_SHARE_WRITE, disposition, FILE_SYNCHRONOUS_IO_NONALERT | options);
}

// The attributes NtQueryFullAttributesFile reads by name, or 0xFFFFFFFF with none.
static uint32_t by_name(const char *path)
{
	ANSI_STRING name;
	const OBJECT_ATTRIBUTES object = named(&name, path);
	FILE_NETWORK_OPEN_INFORMATION information;
	return NT_SUCCESS(NtQueryFullAttributesFile(&object, &information)) ? information.attributes : 0xFFFFFFFF;
}

// Sets attributes, and a write time if given, through a handle opened for it, and logs what's read back.
static void set_and_read(const char *label, const char *path, uint32_t options, uint32_t attributes, uint64_t time)
{
	HANDLE handle;
	NTSTATUS status = open_path(&handle, path, FILE_WRITE_ATTRIBUTES, 0, FILE_OPEN, options);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("attributes: %s: open failed: 0x%08X", label, (unsigned)status);
		return;
	}
	const FILE_BASIC_INFORMATION basic = { .last_write_time = time, .attributes = attributes };
	IO_STATUS_BLOCK io_status;
	status = NtSetInformationFile(handle, &io_status, &basic, sizeof(basic), FILE_BASIC_INFORMATION_CLASS);
	FILE_BASIC_INFORMATION read = { 0 };
	const NTSTATUS query =
		NtQueryInformationFile(handle, &io_status, &read, sizeof(read), FILE_BASIC_INFORMATION_CLASS);
	(void)NtClose(handle);
	xsdk_log_write("attributes: %s: set 0x%02X: 0x%08X; the handle reads 0x%02X (0x%08X), the name 0x%02X", label,
		(unsigned)attributes, (unsigned)status, (unsigned)read.attributes, (unsigned)query, (unsigned)by_name(path));
}

static void probe(const char *kind, const char *path, uint32_t options)
{
	xsdk_log_write("attributes: %s made: the name reads 0x%02X", kind, (unsigned)by_name(path));
	static const uint32_t values[] = { FILE_ATTRIBUTE_READONLY, FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE,
		FILE_ATTRIBUTE_HIDDEN, FILE_ATTRIBUTE_SYSTEM, FILE_ATTRIBUTE_ARCHIVE };
	for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
		set_and_read(kind, path, options, values[i], 0);
		set_and_read(kind, path, options, FILE_ATTRIBUTE_NORMAL, 0);
	}
	set_and_read(kind, path, options, FILE_ATTRIBUTE_READONLY, SOME_NT_TIME);
	set_and_read(kind, path, options, FILE_ATTRIBUTE_NORMAL, 0);
}

static void remove_path(const char *path, uint32_t options)
{
	HANDLE handle;
	if (!NT_SUCCESS(open_path(&handle, path, DELETE, 0, FILE_OPEN, options)))
		return;
	const uint8_t delete_file = 1;
	IO_STATUS_BLOCK io_status;
	const NTSTATUS status =
		NtSetInformationFile(handle, &io_status, &delete_file, sizeof(delete_file), FILE_DISPOSITION_INFORMATION_CLASS);
	(void)NtClose(handle);
	xsdk_log_write("attributes: %s removed: 0x%08X", path, (unsigned)status);
}

int main(void)
{
	static const char file[] = "game:\\probe_attributes.txt";
	static const char folder[] = "game:\\probe_attributes_dir";
	static const char made_read_only[] = "game:\\probe_attributes_ro.txt";
	HANDLE handle;
	NTSTATUS status =
		open_path(&handle, file, GENERIC_WRITE, FILE_ATTRIBUTE_NORMAL, FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE);
	if (NT_SUCCESS(status))
		(void)NtClose(handle);
	probe("file", file, FILE_NON_DIRECTORY_FILE);
	status = open_path(&handle, folder, GENERIC_READ, FILE_ATTRIBUTE_NORMAL, FILE_OPEN_IF, FILE_DIRECTORY_FILE);
	if (NT_SUCCESS(status))
		(void)NtClose(handle);
	probe("folder", folder, FILE_DIRECTORY_FILE);

	status = open_path(
		&handle, made_read_only, GENERIC_WRITE, FILE_ATTRIBUTE_READONLY, FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE);
	if (NT_SUCCESS(status))
		(void)NtClose(handle);
	xsdk_log_write("attributes: made read-only by NtCreateFile: 0x%08X; the name reads 0x%02X", (unsigned)status,
		(unsigned)by_name(made_read_only));
	status = open_path(&handle, made_read_only, GENERIC_WRITE, 0, FILE_OPEN, FILE_NON_DIRECTORY_FILE);
	if (NT_SUCCESS(status))
		(void)NtClose(handle);
	xsdk_log_write("attributes: it opened to write: 0x%08X", (unsigned)status);
	set_and_read("made read-only", made_read_only, FILE_NON_DIRECTORY_FILE, FILE_ATTRIBUTE_NORMAL, 0);

	remove_path(file, FILE_NON_DIRECTORY_FILE);
	remove_path(made_read_only, FILE_NON_DIRECTORY_FILE);
	remove_path(folder, FILE_DIRECTORY_FILE);
	xsdk_log_write("attributes: done");
	return 0;
}
