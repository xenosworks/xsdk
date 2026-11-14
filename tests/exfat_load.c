#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"
#include "xsdk/thread.h"

// Milestone 77 step 3 (docs/scope/77-exfat.md): loads exfat.xex from beside this .xex as a system DLL, logs the
// status and the handle, and exits; the module stays loaded and mounts the exFAT sticks (plugins/exfat/exfat_module.c).
// Milestone 69's loader with the module's name. Kernel 17559 only, for the process type's place.

enum {
	SPLASH_MS = 1000,
	KERNEL_BUILD = 17559,
	PATH_BYTES = 256,
	ORDINAL_XEX_LOAD_IMAGE = 0x199,
	// XexLoadImage's type for a plugin, as pploader and xcn's loader load theirs (pploader:pploader/Main.cpp:54,
	// 436-440; xcn:xbox360/loader.c:9, 34).
	LOAD_SYSTEM_DLL = 8,
	// The kernel loads a system DLL only for a thread of the system's: its process type, in its thread object, 2
	// (xcn:xbox360/loader.c:9-12; xenia:src/xenia/kernel/kernel_state.h:56-58).
	KTHREAD_PROCESS_TYPE = 0x73,
	PROCESS_TYPE_SYSTEM = 2,
	// The processor control region's thread object (xenia:src/xenia/kernel/xthread.h:81).
	PCR_CURRENT_THREAD = 0x100,
	// Long enough for the module's thread to log that it loaded before the title ends.
	SETTLE_MS = 2000,
};
static const char MODULE_NAME[] = "exfat.xex";

typedef NTSTATUS (*XexLoadImageCall)(const char *path, uint32_t type, uint32_t version, HANDLE *module);

static volatile uint8_t *process_type(void)
{
	uint32_t region;
	__asm__ volatile("mr %0, 13" : "=r"(region));
	const uint32_t thread = *(volatile uint32_t *)(uintptr_t)(region + PCR_CURRENT_THREAD);
	return (volatile uint8_t *)(uintptr_t)(thread + KTHREAD_PROCESS_TYPE);
}

// A title importing XexLoadImage doesn't start under Aurora, so it's looked up here (xcn:xbox360/loader.c:7-8).
static XexLoadImageCall find_load_image(void)
{
	HANDLE kernel;
	void *address = NULL;
	if (XexGetModuleHandle("xboxkrnl.exe", &kernel) != 0) {
		xsdk_log_write("exfat_load: XexGetModuleHandle(xboxkrnl.exe) failed");
		return NULL;
	}
	const NTSTATUS status = XexGetProcedureAddress(kernel, ORDINAL_XEX_LOAD_IMAGE, &address);
	if (!NT_SUCCESS(status) || !address) {
		xsdk_log_write("exfat_load: XexGetProcedureAddress(XexLoadImage) failed: 0x%08X", (unsigned)status);
		return NULL;
	}
	return (XexLoadImageCall)address;
}

int main(void)
{
	(void)xsdk_splash_show(SPLASH_MS);
	xsdk_log_write("exfat_load: milestone 77 step 3, kernel %u.%u.%u.%u", XboxKrnlVersion->major,
		XboxKrnlVersion->minor, XboxKrnlVersion->build, XboxKrnlVersion->qfe);
	if (XboxKrnlVersion->build != KERNEL_BUILD) {
		xsdk_log_write("exfat_load: the process type's place is kernel %u's; not loading", KERNEL_BUILD);
		return 1;
	}
	char path[PATH_BYTES];
	const char *const self = ExLoadedImageName ? ExLoadedImageName : "";
	const char *const slash = strrchr(self, '\\');
	if (!slash || snprintf(path, sizeof path, "%.*s\\%s", (int)(slash - self), self, MODULE_NAME) >= (int)sizeof path) {
		xsdk_log_write("exfat_load: no folder in this .xex's path, %s", self);
		return 1;
	}
	HANDLE module;
	if (XexGetModuleHandle(MODULE_NAME, &module) == 0) {
		const LDR_DATA_TABLE_ENTRY *const entry = (const LDR_DATA_TABLE_ENTRY *)(uintptr_t)module;
		xsdk_log_write("exfat_load: %s is loaded already, handle 0x%08X, image 0x%08X, load count %u; not loading",
			MODULE_NAME, (unsigned)module, (unsigned)entry->image_base, entry->load_count);
		return 1;
	}
	const XexLoadImageCall load_image = find_load_image();
	if (!load_image)
		return 1;
	volatile uint8_t *const type = process_type();
	const uint8_t old_type = *type;
	xsdk_log_write("exfat_load: loading %s, process type %u for the call", path, old_type);
	module = 0;
	*type = PROCESS_TYPE_SYSTEM;
	const NTSTATUS status = load_image(path, LOAD_SYSTEM_DLL, 0, &module);
	*type = old_type;
	xsdk_log_write("exfat_load: XexLoadImage: 0x%08X, handle 0x%08X", (unsigned)status, (unsigned)module);
	if (!NT_SUCCESS(status))
		return 1;
	const LDR_DATA_TABLE_ENTRY *const entry = (const LDR_DATA_TABLE_ENTRY *)(uintptr_t)module;
	xsdk_log_write("exfat_load: image 0x%08X-0x%08X, load count %u; exiting, the module staying",
		(unsigned)entry->image_base, (unsigned)(entry->image_base + entry->image_size), entry->load_count);
	(void)xsdk_thread_sleep(SETTLE_MS);
	return 0;
}
