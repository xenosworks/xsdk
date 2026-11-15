#include "exit_private.h"

#include <errno.h>
#include <stdint.h>
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

enum {
	// The title is the only process, with this id.
	TITLE_PID = 1,
	// Callers logged when a signal ends the title.
	CALLERS_LOGGED = 16,
	// Where the KPCR, which r13 holds, keeps the stack's base, its highest address
	// (xenia:src/xenia/kernel/xthread.h:73-80).
	KPCR_STACK_BASE = 0x70,
};

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

// The return addresses above kill's frame, for addr2line, as an abort says nothing of where it came from. A frame's
// first word is its back chain, and the frame above keeps its caller's LR in its second word (GCC's stw r0,N+4(r1)
// after stwu r1,-N(r1)). Stops at the stack's base or a chain that doesn't climb.
static void log_callers(void)
{
	const uint8_t *pcr;
	__asm__ volatile("mr %0, 13" : "=r"(pcr));
	const uintptr_t base = *(const volatile uint32_t *)(pcr + KPCR_STACK_BASE);
	const uint32_t *frame = __builtin_frame_address(0);
	char line[CALLERS_LOGGED * 9 + 1] = "";
	size_t used = 0;
	for (unsigned i = 0; i < CALLERS_LOGGED; i++) {
		const uint32_t *above = (const uint32_t *)(uintptr_t)frame[0];
		if ((uintptr_t)above <= (uintptr_t)frame || (uintptr_t)above >= base)
			break;
		used += (size_t)snprintf(line + used, sizeof line - used, " %08X", (unsigned)above[1]);
		frame = above;
	}
	xsdk_log_write("exit: called from%s", line);
}

// abort() raises SIGABRT through kill; with no handler, end the title as a signal would.
int kill(pid_t pid, int signal)
{
	if (pid != TITLE_PID) {
		errno = ESRCH;
		return -1;
	}
	log_callers();
	xsdk_log_write("exit: signal %d", signal);
	_exit(128 + signal);
}
