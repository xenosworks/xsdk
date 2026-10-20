// hello: the log, the display and a controller. The screen's colour follows the sticks and triggers, and A rumbles.
// It runs until Back, or until the title is left through the Guide's Xbox Home.

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
	// The Guide's first opening held a frame up 2.7 s (docs/HARDWARE.md, "The Guide, milestone 37").
	BEGIN_TIMEOUT_MS = 5000,
	FENCE_TIMEOUT_MS = 1000,
	RUMBLE_SPEED = 0x8000,
	MAX_WIDTH = 1280,
};

static void check_first_frame(uint32_t address, uint32_t width, uint32_t height, uint32_t argb);
static void log_checks(void);

// The left stick's x sets red and the right stick's y green, each 0 at one end and 255 at the other; the right trigger
// sets blue, and the left trigger dims all three.
static uint32_t colour_of(const INPUT_GAMEPAD *pad)
{
	const uint32_t dim = 255 - pad->left_trigger;
	const uint32_t red = (uint32_t)(pad->thumb_lx + 32768) >> 8;
	const uint32_t green = (uint32_t)(pad->thumb_ry + 32768) >> 8;
	const uint32_t blue = pad->right_trigger;
	return 0xFF000000 | red * dim / 255 << 16 | green * dim / 255 << 8 | blue * dim / 255;
}

static void set_rumble(bool on)
{
	const INPUT_VIBRATION vibration = { on ? RUMBLE_SPEED : 0, on ? RUMBLE_SPEED : 0 };
	const uint32_t result = XamInputSetState(0, 0, &vibration);
	if (result != ERROR_SUCCESS)
		xsdk_log_write("hello: XamInputSetState failed: 0x%08X", (unsigned)result);
}

// Each frame reads controller 1, then resolves eDRAM into the next front buffer and shows it. A resolve copies eDRAM,
// then clears it, so each frame shows the colour the frame before cleared to.
static void run(uint32_t width, uint32_t height)
{
	uint32_t last_result = ERROR_SUCCESS;
	bool rumbling = false;
	uint32_t cleared = 0;
	for (uint32_t frame = 0;; frame++) {
		INPUT_STATE input = { 0 };
		const uint32_t result = XamInputGetState(0, 0, &input);
		// Logged when it changes, not every frame: a controller that isn't there fails every time.
		if (result != last_result)
			xsdk_log_write("hello: XamInputGetState returned 0x%08X", (unsigned)result);
		last_result = result;
		if (result != ERROR_SUCCESS)
			input = (INPUT_STATE){ 0 };
		if (input.gamepad.buttons & INPUT_GAMEPAD_BACK)
			break;
		const bool a = input.gamepad.buttons & INPUT_GAMEPAD_A;
		if (a != rumbling)
			set_rumble(a);
		rumbling = a;

		uint32_t address;
		const NTSTATUS status = xsdk_present_begin(&address, BEGIN_TIMEOUT_MS);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("hello: xsdk_present_begin failed: 0x%08X", (unsigned)status);
			break;
		}
		const uint32_t argb = colour_of(&input.gamepad);
#ifdef SAMPLE_BREAK
		// Broken on purpose: the screen clears to another colour than the one the check looks for.
		xsdk_gpu_resolve(address, width, height, argb ^ 0x00FFFFFF);
#else
		xsdk_gpu_resolve(address, width, height, argb);
#endif
		// Frame 0 copies whatever eDRAM held; frame 1, the first colour cleared.
		if (frame == 1)
			check_first_frame(address, width, height, cleared);
		xsdk_present_end();
		cleared = argb;
	}
	if (rumbling)
		set_rumble(false);
}

int main(void)
{
	xsdk_log_write("hello: move the sticks and triggers to change the colour, hold A to rumble, press Back to end");
	const NTSTATUS splash = xsdk_splash_show(SPLASH_MS);
	if (!NT_SUCCESS(splash))
		xsdk_log_write("hello: xsdk_splash_show failed: 0x%08X", (unsigned)splash);
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("hello: xsdk_gpu_open failed: 0x%08X", (unsigned)status);
		return 1;
	}
	uint32_t width = 0;
	uint32_t height = 0;
	status = xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &width, &height);
	if (!NT_SUCCESS(status)) {
		// The title's exit gives the GPU back.
		xsdk_log_write("hello: xsdk_present_open failed: 0x%08X", (unsigned)status);
		return 1;
	}
	xsdk_log_write("hello: the display is %ux%u", (unsigned)width, (unsigned)height);
	run(width, height);
	xsdk_present_close();
	status = xsdk_gpu_close();
	if (!NT_SUCCESS(status))
		xsdk_log_write("hello: xsdk_gpu_close failed: 0x%08X", (unsigned)status);
	xsdk_log_write("hello: Back pressed, done");
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
// download untiles it, and holds each pixel's bytes B, G, R, then A (docs/HARDWARE.md, "A title's buffer on screen").
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

// Once the GPU has finished the frame, its corners and centre hold the colour cleared before it.
static void check_first_frame(uint32_t address, uint32_t width, uint32_t height, uint32_t argb)
{
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	if (!NT_SUCCESS(status))
		xsdk_log_write("hello: xsdk_gpu_wait failed: 0x%08X", (unsigned)status);
	const uint32_t points[][2] = { { 0, 0 }, { width / 2, height / 2 }, { width - 1, height - 1 } };
	bool held = NT_SUCCESS(status);
	for (uint32_t i = 0; i < sizeof points / sizeof points[0] && held; i++) {
		const uint32_t pixel = frame_pixel(address, width, height, points[i][0], points[i][1]);
		if (pixel != argb)
			xsdk_log_write("hello: pixel (%u, %u) held %08X, not %08X", (unsigned)points[i][0], (unsigned)points[i][1],
				(unsigned)pixel, (unsigned)argb);
		held = pixel == argb;
	}
	xsdk_log_write("hello: the first frame cleared to %08X", (unsigned)argb);
	check("hello: the first resolved frame holds the colour it was cleared to", held);
}
