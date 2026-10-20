// sound: a tone through the kernel's audio render driver, its pitch on the triggers: the right one raises it an octave,
// the left one lowers it an octave. The logo stays up while it plays. It runs until Back, or until the title is left
// through the Guide's Xbox Home.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"
#include "xsdk/thread.h"

// A frame is 256 samples of each of 6 channels, big-endian floats a channel after another, at 48 kHz: front left,
// front right, centre, low frequency, back left, back right (docs/HARDWARE.md, "Runtime, milestone 2").
enum {
	FRAME_CHANNELS = 6,
	CHANNEL_SAMPLES = 256,
	SAMPLE_RATE_HZ = 48000,
	FRAME_FLOATS = FRAME_CHANNELS * CHANNEL_SAMPLES,
	FRAME_BYTES = FRAME_FLOATS * 4,
	CHANNEL_FRONT_LEFT = 0,
	CHANNEL_FRONT_RIGHT = 1,
	// The kernel reads the frame it took last until it asks for the next, so the callback fills the other.
	FRAMES = 2,
	// A period of the tone, as float bits, so the callback copies words and does no floating point of its own.
	WAVE_SAMPLES = 1024,
	WAVE_SHIFT = 22,
	POLL_MS = 16,
	CHECK_MS = 1000,
};

static const float BASE_HZ = 440.0f;
static const float AMPLITUDE = 0.25f;

static uint32_t wave[WAVE_SAMPLES];
static float *frames;
// Written by the main thread, read by the callback.
static uint32_t driver;
static uint32_t phase_step;
// Written by the callback, read by the main thread.
static uint32_t submitted_count;
static uint32_t failed_count_submits;

static void check_frames_submitted(uint32_t ms);
static void log_checks(void);

// The kernel calls it on its audio thread each time it wants a frame, about 188 times a second.
static void play_frame(const uint32_t *context)
{
	(void)context;
	static uint32_t phase;
	static uint32_t next;
	const uint32_t current = __atomic_load_n(&driver, __ATOMIC_ACQUIRE);
	// The first calls can come before registering has returned the driver.
	if (!current)
		return;
	uint32_t *const frame = (uint32_t *)(frames + next * FRAME_FLOATS);
	next = (next + 1) % FRAMES;
	const uint32_t step = __atomic_load_n(&phase_step, __ATOMIC_RELAXED);
	// The other channels stay silent, as start_audio left them.
	for (uint32_t sample = 0; sample < CHANNEL_SAMPLES; sample++) {
		const uint32_t value = wave[phase >> WAVE_SHIFT];
		frame[CHANNEL_FRONT_LEFT * CHANNEL_SAMPLES + sample] = value;
		frame[CHANNEL_FRONT_RIGHT * CHANNEL_SAMPLES + sample] = value;
		phase += step;
	}
#ifdef SAMPLE_BREAK
	// Broken on purpose: the frame is never handed to the kernel, so nothing plays.
#else
	if (XAudioSubmitRenderDriverFrame(current, (const float *)frame) == ERROR_SUCCESS)
		__atomic_fetch_add(&submitted_count, 1, __ATOMIC_RELAXED);
	else
		__atomic_fetch_add(&failed_count_submits, 1, __ATOMIC_RELAXED);
#endif
}

// The phase is a 32-bit fraction of a period, which wraps by itself; each sample moves it on by hz / 48000.
static void set_pitch(const INPUT_GAMEPAD *pad)
{
	const float hz = BASE_HZ * powf(2.0f, (pad->right_trigger - pad->left_trigger) / 255.0f);
	__atomic_store_n(&phase_step, (uint32_t)(hz / SAMPLE_RATE_HZ * 4294967296.0f), __ATOMIC_RELAXED);
}

static bool start_audio(void)
{
	for (uint32_t i = 0; i < WAVE_SAMPLES; i++) {
		const float value = AMPLITUDE * sinf(2.0f * (float)M_PI * (float)i / WAVE_SAMPLES);
		memcpy(&wave[i], &value, sizeof value);
	}
	// Never freed: a callback can still be running as unregistering returns, and the title's exit frees it.
	frames = MmAllocatePhysicalMemoryEx(0, FRAMES * FRAME_BYTES, PAGE_READWRITE, 0, UINT32_MAX, 0);
	if (!frames) {
		xsdk_log_write("sound: MmAllocatePhysicalMemoryEx failed");
		return false;
	}
	memset(frames, 0, FRAMES * FRAME_BYTES);
	const INPUT_GAMEPAD rest = { 0 };
	set_pitch(&rest);
	const XAUDIO_RENDER_DRIVER_CLIENT client = { .callback = play_frame, .context = 0 };
	uint32_t registered = 0;
	const uint32_t result = XAudioRegisterRenderDriverClient(&client, &registered);
	if (result != ERROR_SUCCESS) {
		xsdk_log_write("sound: XAudioRegisterRenderDriverClient failed: 0x%08X", (unsigned)result);
		return false;
	}
	__atomic_store_n(&driver, registered, __ATOMIC_RELEASE);
	return true;
}

static void stop_audio(void)
{
	const uint32_t registered = __atomic_exchange_n(&driver, 0, __ATOMIC_ACQ_REL);
	const uint32_t result = XAudioUnregisterRenderDriverClient(registered);
	if (result != ERROR_SUCCESS)
		xsdk_log_write("sound: XAudioUnregisterRenderDriverClient failed: 0x%08X", (unsigned)result);
	const uint32_t failed = __atomic_load_n(&failed_count_submits, __ATOMIC_RELAXED);
	if (failed)
		xsdk_log_write("sound: XAudioSubmitRenderDriverFrame failed %u times", (unsigned)failed);
}

static uint32_t elapsed_ms(const struct timespec *start)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)((now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000);
}

// Reads controller 1 about 60 times a second, until Back.
static void run(const struct timespec *start)
{
	uint32_t last_result = ERROR_SUCCESS;
	bool checked = false;
	for (;;) {
		INPUT_STATE input = { 0 };
		const uint32_t result = XamInputGetState(0, 0, &input);
		// Logged when it changes, not every time: a controller that isn't there fails every time.
		if (result != last_result)
			xsdk_log_write("sound: XamInputGetState returned 0x%08X", (unsigned)result);
		last_result = result;
		if (result != ERROR_SUCCESS)
			input = (INPUT_STATE){ 0 };
		if (input.gamepad.buttons & INPUT_GAMEPAD_BACK)
			return;
		set_pitch(&input.gamepad);
		const uint32_t ms = elapsed_ms(start);
		if (!checked && ms >= CHECK_MS) {
			check_frames_submitted(ms);
			checked = true;
		}
		const NTSTATUS status = xsdk_thread_sleep(POLL_MS);
		if (!NT_SUCCESS(status))
			xsdk_log_write("sound: xsdk_thread_sleep failed: 0x%08X", (unsigned)status);
	}
}

int main(void)
{
	xsdk_log_write("sound: a tone; the right trigger raises it, the left lowers it; press Back to end");
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("sound: xsdk_gpu_open failed: 0x%08X", (unsigned)status);
		return 1;
	}
	// The logo, left up until the end.
	status = xsdk_splash_begin();
	if (!NT_SUCCESS(status))
		xsdk_log_write("sound: xsdk_splash_begin failed: 0x%08X", (unsigned)status);
	if (!start_audio())
		return 1;
	struct timespec start;
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	run(&start);
	stop_audio();
	xsdk_splash_end();
	status = xsdk_gpu_close();
	if (!NT_SUCCESS(status))
		xsdk_log_write("sound: xsdk_gpu_close failed: 0x%08X", (unsigned)status);
	xsdk_log_write("sound: done");
	log_checks();
	return 0;
}

// The checks: what the title can tell of itself from the log, as it can't hear the speakers.

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

// The kernel took a frame every 256 samples at 48 kHz, within a tenth, over the first second or so.
static void check_frames_submitted(uint32_t ms)
{
	const uint32_t submitted = __atomic_load_n(&submitted_count, __ATOMIC_RELAXED);
	const uint32_t expected = ms * SAMPLE_RATE_HZ / CHANNEL_SAMPLES / 1000;
	xsdk_log_write("sound: %u frames submitted in about %u ms, %u expected", (unsigned)submitted, (unsigned)ms,
		(unsigned)expected);
	check("sound: the kernel took frames at 48 kHz", submitted >= expected * 9 / 10 && submitted <= expected * 11 / 10);
}
