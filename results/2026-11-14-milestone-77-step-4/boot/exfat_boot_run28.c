#include <stdint.h>

#include "xsdk/kernel.h"
#include "xsdk/plugin.h"

// Milestone 77 step 4 (results/2026-11-14-milestone-77-step-4, run 28): the module the kernel patch loads at boot is
// this small loader, which waits until xeBuild's stub has loaded launch.xex (it clears the word at 0x8010BFEC then)
// and loads the file system module itself. A system image of that module's size loaded before launch.xex stops the
// init thread in DashLaunch's XexHook (runs 24 to 27); this run asks whether one loaded after it does not.

static const char MODULE_PATH[] = "\\Device\\Harddisk0\\Partition1\\XSDK\\exfat\\exfat_fs.xex";
static const char LOG_PATH[] = "\\Device\\Harddisk0\\Partition1\\XSDK\\exfat\\exfat.log";

static void sleep_ms(uint32_t milliseconds)
{
	const int64_t interval = -(int64_t)milliseconds * 10000;
	(void)KeDelayExecutionThread(KERNEL_MODE, 0, &interval);
}

void plugin_main(HANDLE module)
{
	(void)module;
	while (*(volatile uint32_t *)(uintptr_t)0x8010BFEC)
		sleep_ms(50);
	HANDLE loaded = 0;
	const NTSTATUS status = XexLoadImage(MODULE_PATH, 8, 0, &loaded);
	xsdk_plugin_log_open(LOG_PATH);
	xsdk_plugin_log("exfat: boot loader: exfat_fs.xex load 0x%08X, handle %08X", (unsigned)status, (unsigned)loaded);
}
