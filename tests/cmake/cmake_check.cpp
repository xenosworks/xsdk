#include <stdexcept>
#include <string_view>
#include <thread>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"

namespace
{

enum { SPLASH_MS = 2000 };

int passed_count;
int failed_count;
bool constructed;

struct Constructed {
	Constructed()
	{
		constructed = true;
	}
};

const Constructed constructed_at_start;

void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	if (passed)
		passed_count++;
	else
		failed_count++;
}

bool caught()
{
	try {
		throw std::runtime_error("thrown");
	} catch (const std::runtime_error &) {
		return true;
	}
	return false;
}

bool joined()
{
	bool ran = false;
	std::thread thread([&ran] { ran = true; });
	thread.join();
	return ran;
}

// The kernel finds xsdk_add_xex's NAME as a resource named after the title ID, an XDBF database holding it.
bool named()
{
	HANDLE module = 0;
	const uint32_t found = XexGetModuleHandle(nullptr, &module);
	const void *data = nullptr;
	uint32_t size = 0;
	const NTSTATUS status = found == 0 ? XexGetModuleSection(module, TITLE_ID_HEX, &data, &size) : 0;
	xsdk_log_write("cmake: XexGetModuleHandle 0x%08X, XexGetModuleSection(%s) 0x%08X, %u bytes", (unsigned)found,
		TITLE_ID_HEX, (unsigned)status, (unsigned)size);
	if (found != 0 || !NT_SUCCESS(status) || !data)
		return false;
	const std::string_view database(static_cast<const char *>(data), size);
	return database.starts_with("XDBF") && database.find(TITLE_NAME) != std::string_view::npos;
}

} // namespace

int main()
{
	xsdk_log_write("cmake check title");
	check("splash: the vulkan4xenon logo shown", NT_SUCCESS(xsdk_splash_show(SPLASH_MS)));
	check("cmake: a static constructor ran before main", constructed);
	check("cmake: an exception was thrown and caught", caught());
	check("cmake: a std::thread ran and joined", joined());
	check("cmake: the .xex holds the title name xsdk_add_xex gave it", named());
	xsdk_log_write("tests: %d passed, %d failed", passed_count, failed_count);
	return failed_count;
}
