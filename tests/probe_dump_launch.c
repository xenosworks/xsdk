#include <stdint.h>
#include <string.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"

// Milestone 77 step 4 (results/2026-11-14-milestone-77-step-4, run 27): copies the loaded image of DashLaunch's
// launch.xex, as the kernel has it at its base, to the HDD, so its code can be read against the return addresses on
// the init thread's stack from the boot that hangs. Run from the dash, where launch.xex is loaded and resident.

static const char OUT_PATH[] = "\\Device\\Harddisk0\\Partition1\\XSDK\\launch_image.bin";
static uint8_t copy[256 * 1024];

int main(void)
{
	HANDLE module = 0;
	if (!NT_SUCCESS((NTSTATUS)XexGetModuleHandle("launch.xex", &module))) {
		xsdk_log_write("launch.xex not loaded");
		return 1;
	}
	const LDR_DATA_TABLE_ENTRY *const entry = (const LDR_DATA_TABLE_ENTRY *)(uintptr_t)module;
	xsdk_log_write("launch.xex image 0x%08X-0x%08X entry 0x%08X flags 0x%08X", (unsigned)entry->image_base,
		(unsigned)(entry->image_base + entry->image_size), (unsigned)entry->entry_point, (unsigned)entry->flags);
	if (entry->image_size > sizeof copy) {
		xsdk_log_write("image larger than the copy buffer");
		return 1;
	}
	memcpy(copy, (const void *)(uintptr_t)entry->image_base, entry->image_size);
	const ANSI_STRING name = { sizeof OUT_PATH - 1, sizeof OUT_PATH, OUT_PATH };
	const OBJECT_ATTRIBUTES attributes = { .name = &name };
	HANDLE file;
	IO_STATUS_BLOCK io_status;
	NTSTATUS status = NtCreateFile(&file, GENERIC_WRITE | SYNCHRONIZE, &attributes, &io_status, NULL,
		FILE_ATTRIBUTE_NORMAL, 0, FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("create 0x%08X", (unsigned)status);
		return 1;
	}
	status = NtWriteFile(file, 0, NULL, NULL, &io_status, copy, entry->image_size, NULL);
	xsdk_log_write("write 0x%08X, %u bytes", (unsigned)status, (unsigned)io_status.information);
	(void)NtClose(file);
	return 0;
}
