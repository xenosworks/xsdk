#include "xsdk/terminate.h"

#include <stdbool.h>

#include "terminate_private.h"
#include "xsdk/kernel.h"

// Xbox Home's hook: the kernel's title-terminate notification, whose routine here runs the title's.

// Xenia keeps the priority and says nothing of what it means (xenia:src/xenia/kernel/kernel_state.cc:153-160).
enum { NOTIFY_PRIORITY = 0 };

// In the kernel's list while registered, and in the title's memory, which the kernel frees once the routines return.
static EX_TITLE_TERMINATE_REGISTRATION registration;
static void (*title_routine)(void);
// Set by the first xsdk_on_terminate; registered while the kernel has it in its list.
static bool claimed;
static bool registered;

static void unregister(void)
{
	if (__atomic_exchange_n(&registered, false, __ATOMIC_ACQ_REL))
		ExRegisterTitleTerminateNotification(&registration, 0);
}

// Runs on the console's own thread, with the title's frozen (docs/HARDWARE.md).
static void on_terminate(void)
{
	void (*const routine)(void) = __atomic_load_n(&title_routine, __ATOMIC_ACQUIRE);
	if (routine)
		routine();
	// Left in place, the registration froze the console as the next title started (docs/HARDWARE.md).
	unregister();
}

void xsdk_on_terminate(void (*routine)(void))
{
	__atomic_store_n(&title_routine, routine, __ATOMIC_RELEASE);
	if (__atomic_exchange_n(&claimed, true, __ATOMIC_ACQ_REL))
		return;
	registration.notification_routine = on_terminate;
	registration.priority = NOTIFY_PRIORITY;
	// Marked first, so the routine takes the registration out even if it runs as soon as it's in.
	__atomic_store_n(&registered, true, __ATOMIC_RELEASE);
	ExRegisterTitleTerminateNotification(&registration, 1);
}

void xsdk_terminate_close(void)
{
	unregister();
}
