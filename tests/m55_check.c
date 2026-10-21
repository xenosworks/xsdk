#include "tests.h"

#include "xsdk/log.h"

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
	xsdk_log_write("milestone 55: objects, threads, semaphores, pages and written code");
	test_pthread();
	test_objects();
	test_thread_control();
	test_semaphore();
	test_pages();
	test_cpp_threads();
	xsdk_log_write("milestone 55: %u passed, %u failed", passed_count, failed_count);
	return failed_count != 0;
}
