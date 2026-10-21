// sound: a tone fed to the kernel's audio driver through xsdk_audio_*, its pitch on the triggers: the right one raises
// it an octave, the left one lowers it an octave. The logo stays up while it plays. It runs until Back, or until the
// title is left through the Guide's Xbox Home.

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "xsdk/audio.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"
#include "xsdk/thread.h"

enum {
	// A period of the tone, looked up by the phase's top bits.
	WAVE_SAMPLES = 1024,
	WAVE_SHIFT = 22,
	POLL_MS = 16,
	CHECK_MS = 1000,
	// A wait for a free slot gives up after this, to try again.
	FEED_WAIT_MS = 100,
	FEEDER_STACK_BYTES = 64 * 1024,
};

static const float BASE_HZ = 440.0f;
static const float AMPLITUDE = 0.25f;
// 100 ns units, relative.
static const int64_t JOIN_TIMEOUT = -50000000;

static float wave[WAVE_SAMPLES];
// Written by the main thread, read by the feeder.
static uint32_t phase_step;

static void check_frames_played(uint32_t ms);
static void log_checks(void);

// Fills a frame: the tone on the front channels, silence on the others.
static void fill(float *frame, uint32_t *phase)
{
	volatile float *const out = frame;
	const uint32_t step = __atomic_load_n(&phase_step, __ATOMIC_RELAXED);
	for (uint32_t sample = 0; sample < XSDK_AUDIO_FRAME_SAMPLES; sample++) {
		const float value = wave[*phase >> WAVE_SHIFT];
		for (uint32_t channel = 0; channel < XSDK_AUDIO_CHANNELS; channel++) {
			const bool front = channel == XSDK_AUDIO_FRONT_LEFT || channel == XSDK_AUDIO_FRONT_RIGHT;
			out[channel * XSDK_AUDIO_FRAME_SAMPLES + sample] = front ? value : 0.0f;
		}
		*phase += step;
	}
}

// The title's own thread feeds the driver, each wait ending once the kernel has taken a frame, until the close.
static uint32_t feed(void *context)
{
	(void)context;
	uint32_t phase = 0;
	for (;;) {
		float *frame;
		const NTSTATUS status = xsdk_audio_next_frame(FEED_WAIT_MS, &frame);
		if (status == STATUS_IO_TIMEOUT)
			continue;
		if (status != STATUS_SUCCESS)
			return 0;
		fill(frame, &phase);
		xsdk_audio_queue_frame();
#ifdef SAMPLE_BREAK
		// Broken on purpose: one frame queued, then nothing, so the kernel gets silence.
		return 0;
#endif
	}
}

// The phase is a 32-bit fraction of a period, which wraps by itself; each sample moves it on by hz / 48000.
static void set_pitch(const INPUT_GAMEPAD *pad)
{
	const float hz = BASE_HZ * powf(2.0f, (pad->right_trigger - pad->left_trigger) / 255.0f);
	__atomic_store_n(&phase_step, (uint32_t)(hz / XSDK_AUDIO_RATE_HZ * 4294967296.0f), __ATOMIC_RELAXED);
}

static HANDLE start_audio(void)
{
	for (uint32_t i = 0; i < WAVE_SAMPLES; i++)
		wave[i] = AMPLITUDE * sinf(2.0f * (float)M_PI * (float)i / WAVE_SAMPLES);
	const INPUT_GAMEPAD rest = { 0 };
	set_pitch(&rest);
	NTSTATUS status = xsdk_audio_open(0);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("sound: xsdk_audio_open failed: 0x%08X", (unsigned)status);
		return 0;
	}
	HANDLE feeder = 0;
	status = xsdk_thread_create(&feeder, FEEDER_STACK_BYTES, feed, NULL);
	if (!NT_SUCCESS(status))
		xsdk_log_write("sound: xsdk_thread_create failed: 0x%08X", (unsigned)status);
	return feeder;
}

static void stop_audio(HANDLE feeder)
{
	const NTSTATUS status = xsdk_audio_close();
	if (!NT_SUCCESS(status))
		xsdk_log_write("sound: xsdk_audio_close failed: 0x%08X", (unsigned)status);
	const NTSTATUS joined = NtWaitForSingleObjectEx(feeder, USER_MODE, 0, &JOIN_TIMEOUT);
	if (joined != STATUS_SUCCESS)
		xsdk_log_write("sound: the feeder didn't end: 0x%08X", (unsigned)joined);
	(void)NtClose(feeder);
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
			check_frames_played(ms);
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
	struct timespec start;
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	const HANDLE feeder = start_audio();
	if (!feeder)
		return 1;
	run(&start);
	stop_audio(feeder);
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

// The kernel took a frame every 256 samples at 48 kHz, within a tenth, over the first second or so, none of it silence.
static void check_frames_played(uint32_t ms)
{
	XsdkAudioStats stats;
	xsdk_audio_stats(&stats);
	const uint32_t expected = ms * XSDK_AUDIO_RATE_HZ / XSDK_AUDIO_FRAME_SAMPLES / 1000;
	xsdk_log_write("sound: %u frames played and %u silent in about %u ms, %u expected", (unsigned)stats.played,
		(unsigned)stats.silent, (unsigned)ms, (unsigned)expected);
	check("sound: the kernel played the tone at 48 kHz, none of it silence",
		stats.played >= expected * 9 / 10 && stats.played <= expected * 11 / 10 && stats.silent == 0);
}
