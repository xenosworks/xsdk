#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "xsdk/log.h"
#include "xsdk/xam.h"

// Milestone 56's launched title (docs/scope/56-store-networking.md, Verification), as a store launches one: its launch
// data is the path of the title to go back to, then bytes. It logs its argv and the data, and launches that path back
// with the data reversed.

static uint8_t data[XSDK_LAUNCH_DATA_MAX_BYTES];

int main(int argc, char **argv)
{
	for (int i = 0; i < argc; i++)
		xsdk_log_write("launch_target: argv[%d] \"%s\"", i, argv[i]);
	const size_t size = xsdk_launch_data(data, sizeof(data));
	const uint8_t *const end = memchr(data, '\0', size);
	xsdk_log_write("launch_target: %u bytes of launch data, the path to go back to %s", (unsigned)size,
		end ? (const char *)data : "missing");
	if (!end)
		return 1;
	char back[XSDK_LAUNCH_DATA_MAX_BYTES];
	memcpy(back, data, (size_t)(end - data) + 1);
	for (size_t i = 0; i < size / 2; i++) {
		const uint8_t byte = data[i];
		data[i] = data[size - 1 - i];
		data[size - 1 - i] = byte;
	}
	xsdk_log_write("launch_target: launching %s, the data reversed", back);
	(void)xsdk_launch(back, data, size);
	xsdk_log_write("launch_target: xsdk_launch(%s) failed: errno %d", back, errno);
	return 1;
}
