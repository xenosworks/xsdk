#include "xsdk/xam.h"

#include <stdbool.h>
#include <stdint.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"

enum {
	// XN_SYS_UI, its parameter 1 as the Guide or a dialog opens and 0 as it closes (docs/HARDWARE.md, "The Guide shows
	// through VdSwap"; xenia:src/xenia/kernel/xam/xam_ui.cc:72-98).
	XN_SYS_UI = 9,
	// Xenia's highest version (xenia:src/xenia/kernel/xam/xam_notify.cc:21-28).
	NOTIFY_MAX_VERSION = 10,
};

// Every area's notifications, as tests/guide_check.c listens on the console.
static const uint64_t NOTIFY_ALL_AREAS = UINT64_MAX;

static HANDLE listener;
static bool ui_open;

bool xsdk_system_ui_open(void)
{
	static bool told;
	if (!listener)
		listener = XamNotifyCreateListener(NOTIFY_ALL_AREAS, NOTIFY_MAX_VERSION);
	if (!listener && !told)
		xsdk_log_write("xam: XamNotifyCreateListener failed");
	told |= !listener;
	uint32_t id = 0;
	uint32_t param = 0;
	while (listener && XNotifyGetNext(listener, 0, &id, &param)) {
		if (id == XN_SYS_UI)
			ui_open = param != 0;
	}
	return ui_open;
}
