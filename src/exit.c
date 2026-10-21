#include "exit_private.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>

#include "file_private.h"
#include "terminate_private.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/thread.h"

// The title is the only process, with this id.
enum { TITLE_PID = 1 };

// What _exit launches; empty, it goes back to the dashboard.
static char launch_path[XSDK_PATH_BYTES];

void xsdk_exit_launching(const char *xam_path)
{
	(void)snprintf(launch_path, sizeof launch_path, "%s", xam_path);
	exit(EXIT_SUCCESS);
}

// newlib's exit() ends here after running atexit handlers and static destructors.
void _exit(int status)
{
	// xsdk_on_terminate's routine is for an end from outside, and a registration left in place froze the console.
	xsdk_terminate_close();
	// The callback, the display and the GPU go back to XAM before the title ends (docs/HARDWARE.md).
	xsdk_present_close();
	xsdk_display_close();
	(void)xsdk_gpu_close();
	if (launch_path[0]) {
		xsdk_log_write("exit: status %d, launching %s", status, launch_path);
		xsdk_log_close();
		XamLoaderLaunchTitle(launch_path, 0);
	} else {
		xsdk_log_write("exit: status %d", status);
		xsdk_log_close();
		XamLoaderTerminateTitle();
	}
	// _exit must not return, even if XAM's call ever does.
	for (;;)
		(void)xsdk_thread_sleep(1000);
}

pid_t getpid(void)
{
	return TITLE_PID;
}

// abort() raises SIGABRT through kill; with no handler, end the title as a signal would.
int kill(pid_t pid, int signal)
{
	if (pid != TITLE_PID) {
		errno = ESRCH;
		return -1;
	}
	xsdk_log_write("exit: signal %d", signal);
	_exit(128 + signal);
}
