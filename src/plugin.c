#include "xsdk/plugin.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "xsdk/kernel.h"

uint32_t xsdk_plugin_entry(HANDLE module, uint32_t reason, void *reserved);

enum {
	// DllMain's reason as the loader starts a DLL (xenia:src/xenia/kernel/kernel_state.cc:440-450).
	DLL_PROCESS_ATTACH = 1,
	// A system thread outlives the title, but runs only if given a hardware thread too, as xcn's plugin's is
	// (xcn:docs/INTERNALS.md:105-107, xcn:xbox360/xa_plugin.c:52-54).
	CREATE_SYSTEM_THREAD = 0x2,
	CREATE_HARDWARE_THREAD_SHIFT = 24,
	PLUGIN_HARDWARE_THREAD = 2,
	PLUGIN_STACK_BYTES = 64 * 1024,
	LOG_LINE_BYTES = 512,
};

_Static_assert(offsetof(LDR_DATA_TABLE_ENTRY, load_count) == 0x40, "xenia:src/xenia/kernel/xmodule.h:41");

// src/xsdk_plugin.ld's: where the plugin is linked to load.
extern const char xsdk_plugin_base[];

static HANDLE plugin_module;
// Guards the log, which the plugin's threads write concurrently.
static RTL_CRITICAL_SECTION log_lock;
static const char *log_path;
static uint16_t log_path_bytes;

static uint32_t run(void *context)
{
	(void)context;
	plugin_main(plugin_module);
	// One load left, so the image goes however often it was loaded, as pploader unloads itself
	// (pploader:pploader/Main.cpp:358-369); only where the handle is the loader entry, as the console's is.
	LDR_DATA_TABLE_ENTRY *const entry = (LDR_DATA_TABLE_ENTRY *)(uintptr_t)plugin_module;
	if (entry->image_base == (uint32_t)(uintptr_t)xsdk_plugin_base)
		entry->load_count = 1;
	XexUnloadImageAndExitThread(plugin_module, 0);
	return 0;
}

uint32_t xsdk_plugin_entry(HANDLE module, uint32_t reason, void *reserved)
{
	(void)reserved;
	if (reason != DLL_PROCESS_ATTACH)
		return 1;
	plugin_module = module;
	RtlInitializeCriticalSection(&log_lock);
	HANDLE thread;
	uint32_t thread_id;
	const uint32_t flags = CREATE_SYSTEM_THREAD | (1u << PLUGIN_HARDWARE_THREAD) << CREATE_HARDWARE_THREAD_SHIFT;
	const NTSTATUS status = ExCreateThread(&thread, PLUGIN_STACK_BYTES, &thread_id, NULL, (void *)run, NULL, flags);
	if (!NT_SUCCESS(status)) {
		DbgPrint("xsdk_plugin_entry: ExCreateThread failed: 0x%08X\n", (unsigned)status);
		return 0;
	}
	(void)NtClose(thread);
	return 1;
}

void xsdk_plugin_log_open(const char *path)
{
	uint16_t bytes = 0;
	while (path[bytes])
		bytes++;
	log_path_bytes = bytes;
	log_path = path;
}

static NTSTATUS append(const char *line, uint32_t length_bytes)
{
	const ANSI_STRING name = { .length = log_path_bytes, .maximum_length = log_path_bytes + 1, .pointer = log_path };
	const OBJECT_ATTRIBUTES attributes = { .name = &name };
	IO_STATUS_BLOCK io_status;
	HANDLE file;
	NTSTATUS status = NtCreateFile(&file, GENERIC_WRITE | SYNCHRONIZE, &attributes, &io_status, NULL,
		FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
	if (!NT_SUCCESS(status))
		return status;
	FILE_NETWORK_OPEN_INFORMATION information;
	status =
		NtQueryInformationFile(file, &io_status, &information, sizeof information, FILE_NETWORK_OPEN_INFORMATION_CLASS);
	if (NT_SUCCESS(status))
		status = NtWriteFile(file, 0, NULL, NULL, &io_status, line, length_bytes, &information.end_of_file);
	const NTSTATUS close_status = NtClose(file);
	return NT_SUCCESS(status) ? close_status : status;
}

void xsdk_plugin_log(const char *format, ...)
{
	if (!log_path)
		return;
	char line[LOG_LINE_BYTES];
	va_list args;
	va_start(args, format);
	int32_t length = _vsnprintf(line, sizeof line - 1, format, args);
	va_end(args);
	// A line too long is cut where the kernel stopped writing it.
	if (length < 0 || length > (int32_t)sizeof line - 1)
		length = sizeof line - 1;
	line[length] = '\n';
	RtlEnterCriticalSection(&log_lock);
	const NTSTATUS status = append(line, (uint32_t)length + 1);
	RtlLeaveCriticalSection(&log_lock);
	if (!NT_SUCCESS(status))
		DbgPrint("xsdk_plugin_log: writing %s failed: 0x%08X\n", log_path, (unsigned)status);
}
