#include "tests.h"

#include "xsdk/log.h"
#include "xsdk/splash.h"

enum { SPLASH_MS = 2000 };

static unsigned passed_count;
static unsigned failed_count;

void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	passed_count += passed;
	failed_count += !passed;
}

int main(void)
{
	xsdk_log_write("milestone 55 diagnostics v4: objects, threads, semaphores, pages and written code");
	const NTSTATUS splash = xsdk_splash_show(SPLASH_MS);
	xsdk_log_write("m55 splash: status=0x%08X", (unsigned)splash);
	check("splash: the vulkan4xenon logo shown", NT_SUCCESS(splash));
	test_pthread();
	test_objects();
	test_thread_control();
	test_semaphore();
	test_pages();
	test_cpp_threads();
	xsdk_log_write("milestone 55: %u passed, %u failed", passed_count, failed_count);
	return failed_count != 0;
}
