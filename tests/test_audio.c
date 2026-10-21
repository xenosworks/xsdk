#include <stdbool.h>
#include <stdint.h>

#include "audio_frame.h"
#include "tests.h"
#include "xsdk/audio.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Milestone 58's checks of xsdk_audio_* (docs/scope/58-audio.md, "Verification"). Each feeds from a thread of its own,
// as the call that waits raises its thread.

enum {
	TONE_HZ = 375,
	// The time base's rate (docs/HARDWARE.md, "The time base runs at 49.875 MHz").
	TICKS_PER_MS = 49875,
	// 10 us between two reads of the time base counts as time the busy thread didn't run.
	OFF_TICKS = TICKS_PER_MS / 100,
	FEEDER_HARDWARE_THREAD = 1,
	STACK_BYTES = 64 * 1024,
	FEED_MS = 500,
	// 500 ms of 256-sample frames at 48 kHz.
	FEED_FRAMES = 94,
	FEED_SLACK = 2,
	FILL_MS = 50,
	// The default ring's 8 frames, 43 ms, then 10 frames more.
	STOP_MS = 43 + 53,
	BUSY_MS = 500,
	// At +1 a feeder beside a busy thread woke in 3 us, at 0 up to 20 ms late (docs/HARDWARE.md, milestone 58).
	WAIT_LIMIT_US = 1000,
	BUSY_LOSS_LIMIT_PERMILLE = 10,
	CLOSE_AFTER_MS = 50,
	CLOSE_LIMIT_US = 5000,
	SETTLE_MS = 20,
	WAIT_MS = 500,
	THREAD_WAIT_MS = 5000,
	// The word the console gave at milestone 58's step 2 (docs/HARDWARE.md, "Audio's feeding, milestone 58").
	STEP_2_SPEAKER_CONFIG = 0x00010001,
	UNWRITTEN = 0xA5A5A5A5,
};

// 100 ns units.
static const int64_t MS_100NS = 10000;

typedef struct {
	NTSTATUS status;
	XsdkAudioStats before;
	XsdkAudioStats after;
	uint32_t stop_ticks;
} Run;

typedef struct {
	volatile uint32_t ready;
	volatile uint32_t stop;
	NTSTATUS status;
	uint32_t returned_tick;
} Feeder;

typedef struct {
	uint32_t run_ticks;
	uint32_t off_ticks;
} Busy;

typedef struct {
	uint32_t queued;
	NTSTATUS first;
	NTSTATUS blocked;
	NTSTATUS full;
	XsdkAudioStats stats;
} Depth;

// What threads touch is static, in case one outlasts its join.
static float tone[XSDK_AUDIO_FRAME_FLOATS];
static Run run;
static Feeder feeder;
static Busy busy;
static Depth depth;

static uint32_t time_base(void)
{
	uint32_t ticks;
	__asm__ volatile("mftb %0" : "=r"(ticks));
	return ticks;
}

static uint32_t to_us(uint32_t ticks)
{
	return (uint32_t)((uint64_t)ticks * 1000 / TICKS_PER_MS);
}

// Frames the kernel comes for in ticks, 3 every 16 ms, rounded.
static uint32_t frames_in(uint32_t ticks)
{
	return (uint32_t)(((uint64_t)to_us(ticks) * 3 + 8000) / 16000);
}

// Queues a frame of the tone once a slot is free, written as the driver reads it.
static NTSTATUS feed(uint32_t timeout_ms)
{
	float *frame;
	const NTSTATUS status = xsdk_audio_next_frame(timeout_ms, &frame);
	if (status != STATUS_SUCCESS)
		return status;
	volatile float *const out = frame;
	for (uint32_t i = 0; i < XSDK_AUDIO_FRAME_FLOATS; i++)
		out[i] = tone[i];
	xsdk_audio_queue_frame();
	return STATUS_SUCCESS;
}

static NTSTATUS feed_for(uint32_t ms)
{
	const uint32_t start = time_base();
	NTSTATUS status = STATUS_SUCCESS;
	while (status == STATUS_SUCCESS && time_base() - start < ms * TICKS_PER_MS)
		status = feed(WAIT_MS);
	return status;
}

static bool open_audio(void)
{
	const NTSTATUS status = xsdk_audio_open(0);
	if (!NT_SUCCESS(status))
		xsdk_log_write("audio: xsdk_audio_open failed: 0x%08X", (unsigned)status);
	return NT_SUCCESS(status);
}

static bool close_audio(void)
{
	const NTSTATUS status = xsdk_audio_close();
	if (!NT_SUCCESS(status))
		xsdk_log_write("audio: xsdk_audio_close failed: 0x%08X", (unsigned)status);
	return NT_SUCCESS(status);
}

static HANDLE start(XsdkThreadEntry entry)
{
	HANDLE thread = 0;
	const NTSTATUS status = xsdk_thread_create_on(&thread, STACK_BYTES, entry, NULL, FEEDER_HARDWARE_THREAD);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("audio: xsdk_thread_create_on failed: 0x%08X", (unsigned)status);
		return 0;
	}
	return thread;
}

static bool join(HANDLE thread)
{
	if (!thread)
		return false;
	const int64_t timeout = -THREAD_WAIT_MS * MS_100NS;
	const NTSTATUS status = NtWaitForSingleObjectEx(thread, USER_MODE, 0, &timeout);
	if (status != STATUS_SUCCESS)
		xsdk_log_write("audio: a thread didn't end in %d ms: 0x%08X", THREAD_WAIT_MS, (unsigned)status);
	(void)NtClose(thread);
	return status == STATUS_SUCCESS;
}

// Opens, runs entry on a thread of its own to its end, and closes.
static bool run_feeder(XsdkThreadEntry entry)
{
	if (!open_audio())
		return false;
	const bool ran = join(start(entry));
	return close_audio() && ran;
}

static uint32_t keep_coming(void *context)
{
	(void)context;
	run.status = feed_for(FEED_MS);
	xsdk_audio_stats(&run.after);
	return 0;
}

static void check_frames_keep_coming(void)
{
	run = (Run){ 0 };
	const bool ran = run_feeder(keep_coming);
	const XsdkAudioStats *const stats = &run.after;
	xsdk_log_write(
		"audio: fed for %d ms: %u frames played, %u silent, %u queued at the fewest, a freed slot filled "
		"%u us after at most; feeding returned 0x%08X",
		FEED_MS, (unsigned)stats->played, (unsigned)stats->silent, (unsigned)stats->fewest_queued,
		(unsigned)stats->longest_wait_us, (unsigned)run.status);
	check("audio: a feeder queues a tone for 500 ms: the kernel plays 94 frames of it within 2, none silent",
		ran && run.status == STATUS_SUCCESS && stats->played + FEED_SLACK >= FEED_FRAMES &&
			stats->played <= FEED_FRAMES + FEED_SLACK && stats->silent == 0);
}

static uint32_t underrun(void *context)
{
	(void)context;
	run.status = feed_for(FILL_MS);
	xsdk_audio_stats(&run.before);
	const uint32_t start_tick = time_base();
	(void)xsdk_thread_sleep(STOP_MS);
	run.stop_ticks = time_base() - start_tick;
	xsdk_audio_stats(&run.after);
	return 0;
}

static void check_underruns_counted(void)
{
	run = (Run){ 0 };
	const bool ran = run_feeder(underrun);
	const uint32_t played = run.after.played - run.before.played;
	const uint32_t silent = run.after.silent - run.before.silent;
	// Each frame the kernel came for in the stop that the ring didn't hold was silence.
	const uint32_t expected = frames_in(run.stop_ticks) - played;
	xsdk_log_write(
		"audio: stopped for %u us: %u frames played from the ring, %u silent, %u expected; %u queued at the "
		"fewest",
		(unsigned)to_us(run.stop_ticks), (unsigned)played, (unsigned)silent, (unsigned)expected,
		(unsigned)run.after.fewest_queued);
	check(
		"audio: the feeder stops for its 8 frames and 53 ms more: the stats count 10 silent frames, within 1 of the "
		"frames the kernel came for",
		ran && run.status == STATUS_SUCCESS && silent + 1 >= expected && silent <= expected + 1);
}

// Feeds until stopped or until a wait fails, noting when its last wait returned.
static uint32_t feed_until_stopped(void *context)
{
	(void)context;
	feeder.status = feed(WAIT_MS);
	feeder.returned_tick = time_base();
	__atomic_store_n(&feeder.ready, 1, __ATOMIC_RELEASE);
	while (feeder.status == STATUS_SUCCESS && !__atomic_load_n(&feeder.stop, __ATOMIC_ACQUIRE)) {
		feeder.status = feed(WAIT_MS);
		feeder.returned_tick = time_base();
	}
	return 0;
}

// Busy for BUSY_MS, adding up the gaps of OFF_TICKS or more in which it didn't run.
static uint32_t spin(void *context)
{
	(void)context;
	const uint32_t start_tick = time_base();
	uint32_t previous = start_tick;
	while (previous - start_tick < BUSY_MS * TICKS_PER_MS) {
		const uint32_t now = time_base();
		if (now - previous >= OFF_TICKS)
			busy.off_ticks += now - previous;
		previous = now;
	}
	busy.run_ticks = previous - start_tick;
	return 0;
}

static bool wait_ready(void)
{
	for (uint32_t ms = 0; ms < THREAD_WAIT_MS; ms++) {
		if (__atomic_load_n(&feeder.ready, __ATOMIC_ACQUIRE))
			return true;
		(void)xsdk_thread_sleep(1);
	}
	return false;
}

static void check_beside_busy_thread(void)
{
	feeder = (Feeder){ 0 };
	busy = (Busy){ 0 };
	XsdkAudioStats stats = { 0 };
	bool ran = open_audio();
	if (ran) {
		const HANDLE feeding = start(feed_until_stopped);
		ran = feeding && wait_ready() && join(start(spin));
		xsdk_audio_stats(&stats);
		__atomic_store_n(&feeder.stop, 1, __ATOMIC_RELEASE);
		ran = join(feeding) && ran;
		ran = close_audio() && ran;
	}
	const uint32_t loss_permille = busy.run_ticks ? (uint32_t)((uint64_t)busy.off_ticks * 1000 / busy.run_ticks) : 0;
	xsdk_log_write(
		"audio: beside a busy thread on hardware thread %d: %u frames played, %u silent, a freed slot "
		"filled %u us after at most; the busy thread off %u us in %u ms",
		FEEDER_HARDWARE_THREAD, (unsigned)stats.played, (unsigned)stats.silent, (unsigned)stats.longest_wait_us,
		(unsigned)to_us(busy.off_ticks), (unsigned)to_us(busy.run_ticks) / 1000);
	check(
		"audio: the feeder beside a busy thread on hardware thread 1 for 500 ms: no frame silent, each freed slot "
		"filled within 1 ms, the busy thread losing under 1%",
		ran && feeder.status == STATUS_SUCCESS && stats.played > 0 && stats.silent == 0 &&
			stats.longest_wait_us < WAIT_LIMIT_US && loss_permille < BUSY_LOSS_LIMIT_PERMILLE);
}

// Fills the ring, then waits for the kernel to free a slot and, in the 4.4 ms at least before it comes again
// (docs/HARDWARE.md, "Audio's feeding, milestone 58"), fills that and finds the ring full.
static uint32_t fill_ring(void *context)
{
	(void)context;
	while (depth.queued <= XSDK_AUDIO_FRAMES_AHEAD_MAX && (depth.first = feed(0)) == STATUS_SUCCESS)
		depth.queued++;
	depth.blocked = feed(WAIT_MS);
	depth.queued += depth.blocked == STATUS_SUCCESS;
	float *frame;
	depth.full = xsdk_audio_next_frame(0, &frame);
	xsdk_audio_stats(&depth.stats);
	return 0;
}

static void check_ring_depth(void)
{
	depth = (Depth){ 0 };
	const bool ran = run_feeder(fill_ring);
	const uint32_t waiting = depth.queued - depth.stats.played;
	xsdk_log_write(
		"audio: %u frames queued, %u played, %u waiting as xsdk_audio_next_frame(0) returned 0x%08X; the "
		"first fill ended with 0x%08X, the wait for a slot 0x%08X",
		(unsigned)depth.queued, (unsigned)depth.stats.played, (unsigned)waiting, (unsigned)depth.full,
		(unsigned)depth.first, (unsigned)depth.blocked);
	check("audio: with 8 frames ahead, eight frames queue before xsdk_audio_next_frame(0) times out",
		ran && depth.first == STATUS_IO_TIMEOUT && depth.blocked == STATUS_SUCCESS && depth.full == STATUS_IO_TIMEOUT &&
			waiting == XSDK_AUDIO_FRAMES_AHEAD_DEFAULT);
}

static void check_closing(void)
{
	static const char *const name =
		"audio: xsdk_audio_close returns a waiting feeder within 5 ms, and no callback "
		"counts 20 ms on";
	feeder = (Feeder){ 0 };
	if (!open_audio()) {
		check(name, false);
		return;
	}
	const HANDLE feeding = start(feed_until_stopped);
	(void)xsdk_thread_sleep(CLOSE_AFTER_MS);
	const uint32_t closed_tick = time_base();
	const bool closed = close_audio();
	const bool ended = join(feeding);
	const uint32_t returned_us = to_us(feeder.returned_tick - closed_tick);
	(void)xsdk_thread_sleep(SETTLE_MS);
	XsdkAudioStats settled;
	xsdk_audio_stats(&settled);
	(void)xsdk_thread_sleep(SETTLE_MS);
	XsdkAudioStats later;
	xsdk_audio_stats(&later);
	xsdk_log_write("audio: closed: the feeder's wait returned 0x%08X %u us after; callbacks counted %u, then %u",
		(unsigned)feeder.status, (unsigned)returned_us, (unsigned)(settled.played + settled.silent),
		(unsigned)(later.played + later.silent));
	check(name,
		closed && ended && feeder.status == STATUS_INVALID_HANDLE && returned_us < CLOSE_LIMIT_US &&
			settled.played + settled.silent == later.played + later.silent);
}

static void check_speakers(void)
{
	uint32_t config = UNWRITTEN;
	const uint32_t result = XAudioGetSpeakerConfig(&config);
	XsdkAudioSpeakers speakers = XSDK_AUDIO_SPEAKERS_STEREO;
	const NTSTATUS status = xsdk_audio_speakers(&speakers);
	xsdk_log_write("audio: XAudioGetSpeakerConfig returned 0x%08X, the word %08X; xsdk_audio_speakers 0x%08X, %s",
		(unsigned)result, (unsigned)config, (unsigned)status,
		speakers == XSDK_AUDIO_SPEAKERS_SURROUND ? "5.1" : "stereo");
	check("audio: XAudioGetSpeakerConfig gives 00010001, as at step 2, and xsdk_audio_speakers 5.1 for it (unverified)",
		result == ERROR_SUCCESS && config == STEP_2_SPEAKER_CONFIG && status == STATUS_SUCCESS &&
			speakers == XSDK_AUDIO_SPEAKERS_SURROUND);
}

void test_audio(void)
{
	audio_frame_fill(tone, 1u << CHANNEL_FRONT_LEFT | 1u << CHANNEL_FRONT_RIGHT, TONE_HZ);
	check_frames_keep_coming();
	check_underruns_counted();
	check_beside_busy_thread();
	check_ring_depth();
	check_closing();
	check_speakers();
}
