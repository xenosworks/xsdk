#ifndef XSDK_AUDIO_H
#define XSDK_AUDIO_H

#include <stdint.h>

#include "xsdk/kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

// A frame is 256 samples of each of 6 channels at 48 kHz, big-endian floats a channel after another, in the order
// below (docs/HARDWARE.md, "Runtime, milestone 2").
enum {
	XSDK_AUDIO_CHANNELS = 6,
	XSDK_AUDIO_FRAME_SAMPLES = 256,
	XSDK_AUDIO_RATE_HZ = 48000,
	XSDK_AUDIO_FRAME_FLOATS = XSDK_AUDIO_CHANNELS * XSDK_AUDIO_FRAME_SAMPLES,
	// 43 ms, five times Dusklight's longest wait for its audio lock (docs/HARDWARE.md, milestone 58).
	XSDK_AUDIO_FRAMES_AHEAD_DEFAULT = 8,
	// 85 ms, twice the default.
	XSDK_AUDIO_FRAMES_AHEAD_MAX = 16,
};

enum {
	XSDK_AUDIO_FRONT_LEFT,
	XSDK_AUDIO_FRONT_RIGHT,
	XSDK_AUDIO_CENTER,
	XSDK_AUDIO_LOW_FREQUENCY,
	XSDK_AUDIO_BACK_LEFT,
	XSDK_AUDIO_BACK_RIGHT,
};

// Counted from xsdk_audio_open.
typedef struct {
	// Frames the kernel took from the queue.
	uint32_t played;
	// Silent frames the kernel got instead, from the first frame queued on: the feeder was behind.
	uint32_t silent;
	// The fewest frames queued as the kernel came for one, from the first frame queued on.
	uint32_t fewest_queued;
	// The longest a slot the kernel freed waited for its next frame to be queued.
	uint32_t longest_wait_us;
} XsdkAudioStats;

typedef enum {
	XSDK_AUDIO_SPEAKERS_STEREO,
	XSDK_AUDIO_SPEAKERS_SURROUND,
} XsdkAudioSpeakers;

// Registers the title's one client of the kernel's render driver, with up to frames_ahead frames queued ahead of the
// one it plays; 0 for XSDK_AUDIO_FRAMES_AHEAD_DEFAULT. Silence plays until the first frame is queued.
NTSTATUS xsdk_audio_open(uint32_t frames_ahead);
// Waits up to timeout_ms for a free slot and gives its frame to fill, the same one until it's queued:
// STATUS_IO_TIMEOUT if none came free, STATUS_INVALID_HANDLE once closed. The first call after opening raises the
// calling thread to +1 if it's lower (docs/GUIDE.md, "Audio").
NTSTATUS xsdk_audio_next_frame(uint32_t timeout_ms, float **frame);
// Queues the frame xsdk_audio_next_frame gave, after those queued before it.
void xsdk_audio_queue_frame(void);
void xsdk_audio_stats(XsdkAudioStats *stats);
// Unregisters the client; a thread waiting in xsdk_audio_next_frame returns.
NTSTATUS xsdk_audio_close(void);
// The dashboard's audio output, from XAudioGetSpeakerConfig.
NTSTATUS xsdk_audio_speakers(XsdkAudioSpeakers *speakers);

#ifdef __cplusplus
}
#endif

#endif
