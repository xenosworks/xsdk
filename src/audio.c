#include "xsdk/audio.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

enum {
	FRAME_BYTES = XSDK_AUDIO_FRAME_FLOATS * 4,
	// Frame n of the queue goes in slot n % slots; slots is one more than frames_ahead, for the frame the kernel holds.
	SILENCE_SLOT = XSDK_AUDIO_FRAMES_AHEAD_MAX + 1,
	FRAME_COUNT = SILENCE_SLOT + 1,
	// The time base's rate (docs/HARDWARE.md, "The time base runs at 49.875 MHz").
	TICKS_PER_MS = 49875,
	// At +1 a feeder beside a busy thread woke in 3 us, at 0 up to 20 ms late (docs/HARDWARE.md, milestone 58).
	FEEDER_PRIORITY = 1,
	// The processor control region's hardware thread (xenia:src/xenia/kernel/xthread.h:81-83).
	PCR_CURRENT_PROCESSOR = 0x10C,
};

// Diagnostic for milestone 58's console run, removed once read: the kernel's calls, those before the driver was stored,
// failed submits, the last failure, the last call's hardware thread, the feeder's waits, the last wait's status, and
// where the callback is: 0 out of it, 1 in, 2 submitting, 3 setting the event.
enum {
	CALLS,
	EARLY,
	SUBMIT_FAILURES,
	SUBMIT_RESULT,
	CALL_HARDWARE_THREAD,
	WAITS,
	WAIT_STATUS,
	CALLBACK_STEP,
	AUDIO_DIAGNOSTICS,
};

void xsdk_audio_diagnostics(uint32_t diagnostics[AUDIO_DIAGNOSTICS]);

typedef struct {
	// Kept to the end: a callback can still run after unregistering (xenia:src/xenia/apu/audio_system.cc:120-130).
	float *frames;
	HANDLE event;
	uint32_t event_object;
	// 0 while closed.
	uint32_t driver;
	uint32_t frames_ahead;
	uint32_t slots;
	bool raise_pending;
	// Only the feeder writes queued and longest_wait_ticks; only the callback the rest.
	uint32_t queued;
	uint32_t longest_wait_ticks;
	uint32_t taken;
	uint32_t silent;
	uint32_t fewest_queued;
	// When the count of frames taken reached n, at n % slots.
	uint32_t freed_tick[XSDK_AUDIO_FRAMES_AHEAD_MAX + 1];
	// Diagnostic for milestone 58's console run, removed once read: see xsdk_audio_diagnostics.
	uint32_t diagnostics[AUDIO_DIAGNOSTICS];
} Audio;

static Audio audio;

static uint32_t time_base(void)
{
	uint32_t ticks;
	__asm__ volatile("mftb %0" : "=r"(ticks));
	return ticks;
}

static float *slot_frame(uint32_t slot)
{
	return audio.frames + slot * XSDK_AUDIO_FRAME_FLOATS;
}

// On the kernel's audio thread, every 5.33 ms; integers only (docs/GUIDE.md, "Input and audio through the kernel").
static void callback(const uint32_t *context)
{
	(void)context;
	uint32_t region;
	__asm__ volatile("mr %0, 13" : "=r"(region));
	__atomic_store_n(&audio.diagnostics[CALL_HARDWARE_THREAD],
		*(volatile const uint8_t *)(uintptr_t)(region + PCR_CURRENT_PROCESSOR), __ATOMIC_RELAXED);
	__atomic_fetch_add(&audio.diagnostics[CALLS], 1, __ATOMIC_RELAXED);
	__atomic_store_n(&audio.diagnostics[CALLBACK_STEP], 1, __ATOMIC_RELAXED);
	const uint32_t driver = __atomic_load_n(&audio.driver, __ATOMIC_ACQUIRE);
	// Closed, or not yet stored: Xenia calls back as a client registers (xenia:src/xenia/apu/audio_system.cc:179).
	if (!driver) {
		__atomic_fetch_add(&audio.diagnostics[EARLY], 1, __ATOMIC_RELAXED);
		__atomic_store_n(&audio.diagnostics[CALLBACK_STEP], 0, __ATOMIC_RELAXED);
		return;
	}
	const uint32_t taken = audio.taken;
	const uint32_t queued = __atomic_load_n(&audio.queued, __ATOMIC_ACQUIRE);
	const uint32_t waiting = queued - taken;
	const float *frame = slot_frame(SILENCE_SLOT);
	if (waiting) {
		frame = slot_frame(taken % audio.slots);
		audio.freed_tick[(taken + 1) % audio.slots] = time_base();
		__atomic_store_n(&audio.taken, taken + 1, __ATOMIC_RELEASE);
	} else if (queued) {
		__atomic_store_n(&audio.silent, audio.silent + 1, __ATOMIC_RELAXED);
	}
	if (queued && waiting < audio.fewest_queued)
		__atomic_store_n(&audio.fewest_queued, waiting, __ATOMIC_RELAXED);
	__atomic_store_n(&audio.diagnostics[CALLBACK_STEP], 2, __ATOMIC_RELAXED);
	const uint32_t submitted = XAudioSubmitRenderDriverFrame(driver, frame);
	if (submitted != ERROR_SUCCESS) {
		__atomic_fetch_add(&audio.diagnostics[SUBMIT_FAILURES], 1, __ATOMIC_RELAXED);
		__atomic_store_n(&audio.diagnostics[SUBMIT_RESULT], submitted, __ATOMIC_RELAXED);
	}
	__atomic_store_n(&audio.diagnostics[CALLBACK_STEP], 3, __ATOMIC_RELAXED);
	if (waiting)
		(void)KeSetEvent((void *)(uintptr_t)audio.event_object, 0, 0);
	__atomic_store_n(&audio.diagnostics[CALLBACK_STEP], 0, __ATOMIC_RELAXED);
}

static NTSTATUS create_once(void)
{
	if (audio.frames)
		return STATUS_SUCCESS;
	// Cached, as the driver has read frames since milestone 2 (docs/HARDWARE.md, "Runtime, milestone 2").
	float *const frames = MmAllocatePhysicalMemoryEx(0, FRAME_COUNT * FRAME_BYTES, PAGE_READWRITE, 0, UINT32_MAX, 0);
	if (!frames) {
		xsdk_log_write("xsdk_audio_open: MmAllocatePhysicalMemoryEx failed");
		return STATUS_NO_MEMORY;
	}
	NTSTATUS status = NtCreateEvent(&audio.event, NULL, SYNCHRONIZATION_EVENT, 0);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("xsdk_audio_open: NtCreateEvent failed: 0x%08X", (unsigned)status);
		MmFreePhysicalMemory(0, frames);
		return status;
	}
	status = ObReferenceObjectByHandle(audio.event, 0, &audio.event_object);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("xsdk_audio_open: ObReferenceObjectByHandle failed: 0x%08X", (unsigned)status);
		(void)NtClose(audio.event);
		MmFreePhysicalMemory(0, frames);
		return status;
	}
	volatile uint32_t *const silence = (volatile uint32_t *)(frames + SILENCE_SLOT * XSDK_AUDIO_FRAME_FLOATS);
	for (uint32_t i = 0; i < XSDK_AUDIO_FRAME_FLOATS; i++)
		silence[i] = 0;
	audio.frames = frames;
	return STATUS_SUCCESS;
}

NTSTATUS xsdk_audio_open(uint32_t frames_ahead)
{
	// One client a title (docs/scope/58-audio.md, "Left out").
	assert(!audio.driver);
	if (frames_ahead > XSDK_AUDIO_FRAMES_AHEAD_MAX)
		return STATUS_INVALID_PARAMETER;
	const NTSTATUS status = create_once();
	if (!NT_SUCCESS(status))
		return status;
	audio.frames_ahead = frames_ahead ? frames_ahead : XSDK_AUDIO_FRAMES_AHEAD_DEFAULT;
	audio.slots = audio.frames_ahead + 1;
	audio.raise_pending = true;
	audio.queued = 0;
	audio.longest_wait_ticks = 0;
	audio.taken = 0;
	audio.silent = 0;
	audio.fewest_queued = audio.frames_ahead;
	for (uint32_t i = 0; i < AUDIO_DIAGNOSTICS; i++)
		__atomic_store_n(&audio.diagnostics[i], 0, __ATOMIC_RELAXED);
	const XAUDIO_RENDER_DRIVER_CLIENT client = { .callback = callback, .context = 0 };
	uint32_t driver = 0;
	const uint32_t result = XAudioRegisterRenderDriverClient(&client, &driver);
	if (result != ERROR_SUCCESS) {
		xsdk_log_write("xsdk_audio_open: XAudioRegisterRenderDriverClient failed: 0x%08X", (unsigned)result);
		return STATUS_UNSUCCESSFUL;
	}
	__atomic_store_n(&audio.driver, driver, __ATOMIC_RELEASE);
	return STATUS_SUCCESS;
}

static void raise_feeder(void)
{
	int32_t previous = 0;
	const NTSTATUS status = xsdk_thread_set_priority(XSDK_CURRENT_THREAD, FEEDER_PRIORITY, &previous);
	// A thread the title raised further keeps its priority.
	if (NT_SUCCESS(status) && previous > FEEDER_PRIORITY)
		(void)xsdk_thread_set_priority(XSDK_CURRENT_THREAD, previous, NULL);
}

static uint64_t now_100ns(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 10000000 + (uint64_t)now.tv_nsec / 100;
}

NTSTATUS xsdk_audio_next_frame(uint32_t timeout_ms, float **frame)
{
	if (audio.raise_pending) {
		audio.raise_pending = false;
		raise_feeder();
	}
	const uint64_t start = now_100ns();
	const uint64_t limit = (uint64_t)timeout_ms * 10000;
	for (;;) {
		if (!__atomic_load_n(&audio.driver, __ATOMIC_ACQUIRE))
			return STATUS_INVALID_HANDLE;
		const uint32_t queued = audio.queued;
		if (queued - __atomic_load_n(&audio.taken, __ATOMIC_ACQUIRE) < audio.frames_ahead) {
			*frame = slot_frame(queued % audio.slots);
			return STATUS_SUCCESS;
		}
		const uint64_t elapsed = now_100ns() - start;
		if (elapsed >= limit)
			return STATUS_IO_TIMEOUT;
		// The callback sets the event as it takes a frame, and closing sets it too.
		const int64_t wait = -(int64_t)(limit - elapsed);
		const NTSTATUS status = NtWaitForSingleObjectEx(audio.event, USER_MODE, 0, &wait);
		__atomic_fetch_add(&audio.diagnostics[WAITS], 1, __ATOMIC_RELAXED);
		__atomic_store_n(&audio.diagnostics[WAIT_STATUS], (uint32_t)status, __ATOMIC_RELAXED);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("xsdk_audio_next_frame: NtWaitForSingleObjectEx failed: 0x%08X", (unsigned)status);
			return status;
		}
	}
}

void xsdk_audio_queue_frame(void)
{
	const uint32_t queued = audio.queued;
	assert(queued - __atomic_load_n(&audio.taken, __ATOMIC_ACQUIRE) < audio.frames_ahead);
	// The slot came free as the count of frames taken reached queued - frames_ahead + 1.
	// shortcut: a wait over 86 s wraps the time base's low word; read 64 bits if that matters.
	if (queued >= audio.frames_ahead) {
		const uint32_t waited = time_base() - audio.freed_tick[(queued - audio.frames_ahead + 1) % audio.slots];
		if (waited > audio.longest_wait_ticks)
			__atomic_store_n(&audio.longest_wait_ticks, waited, __ATOMIC_RELAXED);
	}
	__atomic_store_n(&audio.queued, queued + 1, __ATOMIC_RELEASE);
}

void xsdk_audio_stats(XsdkAudioStats *stats)
{
	const uint32_t wait_ticks = __atomic_load_n(&audio.longest_wait_ticks, __ATOMIC_RELAXED);
	*stats = (XsdkAudioStats){
		.played = __atomic_load_n(&audio.taken, __ATOMIC_RELAXED),
		.silent = __atomic_load_n(&audio.silent, __ATOMIC_RELAXED),
		.fewest_queued = __atomic_load_n(&audio.fewest_queued, __ATOMIC_RELAXED),
		.longest_wait_us = (uint32_t)((uint64_t)wait_ticks * 1000 / TICKS_PER_MS),
	};
}

void xsdk_audio_diagnostics(uint32_t diagnostics[AUDIO_DIAGNOSTICS])
{
	for (uint32_t i = 0; i < AUDIO_DIAGNOSTICS; i++)
		diagnostics[i] = __atomic_load_n(&audio.diagnostics[i], __ATOMIC_RELAXED);
}

NTSTATUS xsdk_audio_close(void)
{
	const uint32_t driver = __atomic_exchange_n(&audio.driver, 0, __ATOMIC_ACQ_REL);
	if (!driver)
		return STATUS_INVALID_HANDLE;
	// Wakes a feeder waiting in xsdk_audio_next_frame, to find the driver gone.
	const NTSTATUS status = NtSetEvent(audio.event, NULL);
	const uint32_t result = XAudioUnregisterRenderDriverClient(driver);
	if (result != ERROR_SUCCESS) {
		xsdk_log_write("xsdk_audio_close: XAudioUnregisterRenderDriverClient failed: 0x%08X", (unsigned)result);
		return STATUS_UNSUCCESSFUL;
	}
	if (!NT_SUCCESS(status))
		xsdk_log_write("xsdk_audio_close: NtSetEvent failed: 0x%08X", (unsigned)status);
	return status;
}

NTSTATUS xsdk_audio_speakers(XsdkAudioSpeakers *speakers)
{
	uint32_t config = 0;
	const uint32_t result = XAudioGetSpeakerConfig(&config);
	if (result != ERROR_SUCCESS) {
		xsdk_log_write("xsdk_audio_speakers: XAudioGetSpeakerConfig failed: 0x%08X", (unsigned)result);
		return STATUS_UNSUCCESSFUL;
	}
	// unverified: 5.1 as the high half set; a read with the dashboard set the other way names the bits
	// (docs/HARDWARE.md, "Audio's feeding, milestone 58").
	*speakers = config >> 16 ? XSDK_AUDIO_SPEAKERS_SURROUND : XSDK_AUDIO_SPEAKERS_STEREO;
	return STATUS_SUCCESS;
}
