// The start of a title: the logo, then the GPU and the display, and the screen cleared every frame until Back is
// pressed, or until the title is left through the Guide's Xbox Home. Draw where run() says; xsdk's samples show how.

#include <stdbool.h>
#include <stdint.h>

#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/splash.h"
#include "xsdk/texture.h"

enum {
	SPLASH_MS = 2000,
	BUFFERS = 2,
	SWAP_INTERVAL = 1,
	// The Guide's first opening held a frame up 2.7 s (xsdk:docs/HARDWARE.md, "The Guide, milestone 37").
	BEGIN_TIMEOUT_MS = 5000,
	FENCE_TIMEOUT_MS = 1000,
	CLEAR_ARGB = 0xFF183060,
	MAX_WIDTH = 1280,
};

static void check_first_frame(uint32_t address, uint32_t width, uint32_t height);
static void log_checks(void);

static void run(uint32_t width, uint32_t height)
{
	for (uint32_t frame = 0;; frame++) {
		INPUT_STATE input = { 0 };
		if (XamInputGetState(0, 0, &input) == ERROR_SUCCESS && input.gamepad.buttons & INPUT_GAMEPAD_BACK)
			return;
		uint32_t address;
		const NTSTATUS status = xsdk_present_begin(&address, BEGIN_TIMEOUT_MS);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("title: xsdk_present_begin failed: 0x%08X", (unsigned)status);
			return;
		}
		// Draw here: xsdk_gpu_set_target(width, height), then shaders, vertices and draws, as samples/triangle does.
		// The resolve copies eDRAM into the front buffer, then clears eDRAM for the next frame.
		xsdk_gpu_resolve(address, width, height, CLEAR_ARGB);
		// Frame 0 copies whatever eDRAM held; frame 1, the first clear.
		if (frame == 1)
			check_first_frame(address, width, height);
		xsdk_present_end();
	}
}

int main(void)
{
	xsdk_log_write("title: press Back to end");
	const NTSTATUS splash = xsdk_splash_show(SPLASH_MS);
	if (!NT_SUCCESS(splash))
		xsdk_log_write("title: xsdk_splash_show failed: 0x%08X", (unsigned)splash);
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("title: xsdk_gpu_open failed: 0x%08X", (unsigned)status);
		return 1;
	}
	uint32_t width = 0;
	uint32_t height = 0;
	status = xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &width, &height);
	if (!NT_SUCCESS(status)) {
		// The title's exit gives the GPU back.
		xsdk_log_write("title: xsdk_present_open failed: 0x%08X", (unsigned)status);
		return 1;
	}
	xsdk_log_write("title: the display is %ux%u", (unsigned)width, (unsigned)height);
	run(width, height);
	xsdk_present_close();
	status = xsdk_gpu_close();
	if (!NT_SUCCESS(status))
		xsdk_log_write("title: xsdk_gpu_close failed: 0x%08X", (unsigned)status);
	xsdk_log_write("title: done");
	log_checks();
	return 0;
}

// The checks: what the title can tell of itself from the log, as it can't see the screen.

static uint32_t passed_count;
static uint32_t failed_count;

static void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	passed_count += passed;
	failed_count += !passed;
}

static void log_checks(void)
{
	xsdk_log_write("checks: %u passed, %u failed", (unsigned)passed_count, (unsigned)failed_count);
}

// Pixel (x, y) of a frame the GPU has resolved, as 0xAARRGGBB. The frame lies in memory as a tiled texture does, so a
// download untiles it, and holds each pixel's bytes B, G, R, then A (xsdk:docs/HARDWARE.md, "A title's buffer on
// screen").
static uint32_t frame_pixel(uint32_t address, uint32_t width, uint32_t height, uint32_t x, uint32_t y)
{
	XsdkTexture frame;
	if (width > MAX_WIDTH || !NT_SUCCESS(xsdk_texture_init(&frame, XSDK_FORMAT_A8R8G8B8, width, height, 1, true)))
		return 0;
	xsdk_texture_attach(&frame, xsdk_present_pixels(address));
	static uint32_t row[MAX_WIDTH];
	xsdk_texture_download_rows(&frame, 0, y, 1, row, sizeof row);
	return __builtin_bswap32(row[x]);
}

// Once the GPU has finished the frame, its middle holds the clear colour.
static void check_first_frame(uint32_t address, uint32_t width, uint32_t height)
{
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	if (!NT_SUCCESS(status))
		xsdk_log_write("title: xsdk_gpu_wait failed: 0x%08X", (unsigned)status);
	const uint32_t pixel = frame_pixel(address, width, height, width / 2, height / 2);
	if (pixel != CLEAR_ARGB)
		xsdk_log_write("title: the first frame's middle held %08X, not %08X", (unsigned)pixel, (unsigned)CLEAR_ARGB);
	check("title: the first resolved frame holds the clear colour", NT_SUCCESS(status) && pixel == CLEAR_ARGB);
}
