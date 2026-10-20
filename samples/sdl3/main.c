// sdl3: SDL3's gamepad and audio beside xsdk's drawing, as a port would use them. The left stick sets the screen's
// colour, the right trigger the volume of a tone, and A rumbles. It runs until Back, or until the title is left through
// the Guide's Xbox Home.

#include <SDL3/SDL.h>
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
	TONE_HZ = 440,
	TONE_RATE = 48000,
	TONE_CHANNELS = 2,
	TONE_CHUNK_FRAMES = 256,
	RUMBLE_MS = 10000,
	CHECK_FRAMES = 60,
	MAX_WIDTH = 1280,
};

static const float MAX_AMPLITUDE = 0.25f;

// What SDL's audio thread and the main thread share.
typedef struct {
	float phase;
	SDL_AtomicInt volume;
	SDL_AtomicInt frames;
} Tone;

static void check_first_frame(uint32_t address, uint32_t width, uint32_t height, uint32_t argb);
static void check_devices(SDL_Gamepad *pad, Tone *tone, Uint64 start_ns);
static void log_checks(void);

// Runs on SDL's audio thread whenever the stream wants more of the tone: interleaved stereo floats.
static void SDLCALL make_tone(void *userdata, SDL_AudioStream *stream, int additional_amount, int total_amount)
{
	(void)total_amount;
	Tone *const tone = userdata;
	const float amplitude = MAX_AMPLITUDE * SDL_GetAtomicInt(&tone->volume) / 32767.0f;
	float samples[TONE_CHUNK_FRAMES * TONE_CHANNELS];
	for (int frames = additional_amount / (int)sizeof(float) / TONE_CHANNELS; frames > 0;) {
		const int now = SDL_min(frames, TONE_CHUNK_FRAMES);
		for (int i = 0; i < now; i++) {
			samples[2 * i] = samples[2 * i + 1] = amplitude * SDL_sinf(tone->phase);
			tone->phase += 2.0f * SDL_PI_F * TONE_HZ / TONE_RATE;
			if (tone->phase > 2.0f * SDL_PI_F)
				tone->phase -= 2.0f * SDL_PI_F;
		}
		if (!SDL_PutAudioStreamData(stream, samples, now * TONE_CHANNELS * (int)sizeof(float)))
			return;
		(void)SDL_AddAtomicInt(&tone->frames, now);
		frames -= now;
	}
}

static SDL_AudioStream *open_tone(Tone *tone)
{
	const SDL_AudioSpec spec = { SDL_AUDIO_F32, TONE_CHANNELS, TONE_RATE };
	SDL_AudioStream *const stream =
		SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, make_tone, tone);
	if (!stream) {
		xsdk_log_write("sdl3: SDL_OpenAudioDeviceStream failed: %s", SDL_GetError());
		return NULL;
	}
	if (!SDL_ResumeAudioStreamDevice(stream))
		xsdk_log_write("sdl3: SDL_ResumeAudioStreamDevice failed: %s", SDL_GetError());
	return stream;
}

// SDL announces each gamepad, those there at the start included, with SDL_EVENT_GAMEPAD_ADDED.
static void open_gamepad(SDL_Gamepad **pad, SDL_JoystickID id)
{
	if (*pad)
		return;
#ifdef SAMPLE_BREAK
	// Broken on purpose: the gamepad is never opened, so nothing reads it.
	(void)id;
#else
	*pad = SDL_OpenGamepad(id);
	if (!*pad)
		xsdk_log_write("sdl3: SDL_OpenGamepad failed: %s", SDL_GetError());
#endif
}

// Handles the events waiting; false once Back is pressed.
static bool handle_events(SDL_Gamepad **pad)
{
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		if (event.type == SDL_EVENT_GAMEPAD_ADDED)
			open_gamepad(pad, event.gdevice.which);
		if (event.type == SDL_EVENT_GAMEPAD_REMOVED && *pad && SDL_GetGamepadID(*pad) == event.gdevice.which) {
			SDL_CloseGamepad(*pad);
			*pad = NULL;
		}
		const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
		if (down && event.gbutton.button == SDL_GAMEPAD_BUTTON_BACK)
			return false;
		if ((down || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP) && event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH &&
			*pad && !SDL_RumbleGamepad(*pad, down ? 0xC000 : 0, down ? 0xC000 : 0, down ? RUMBLE_MS : 0))
			xsdk_log_write("sdl3: SDL_RumbleGamepad failed: %s", SDL_GetError());
	}
	return true;
}

// The left stick's x sets red and its y blue, each 0 at one end and 255 at the other.
static uint32_t colour_of(SDL_Gamepad *pad)
{
	const int x = pad ? SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) : 0;
	const int y = pad ? SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) : 0;
	return 0xFF000000 | (uint32_t)(x + 32768) >> 8 << 16 | 0x40 << 8 | (uint32_t)(y + 32768) >> 8;
}

static void run(Tone *tone, Uint64 start_ns, uint32_t width, uint32_t height)
{
	SDL_Gamepad *pad = NULL;
	uint32_t cleared = 0;
	for (uint32_t frame = 0; handle_events(&pad); frame++) {
		SDL_SetAtomicInt(&tone->volume, pad ? SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) : 0);
		uint32_t address;
		const NTSTATUS status = xsdk_present_begin(&address, BEGIN_TIMEOUT_MS);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("sdl3: xsdk_present_begin failed: 0x%08X", (unsigned)status);
			break;
		}
		// A resolve copies eDRAM, then clears it, so each frame shows the colour the frame before cleared to.
		const uint32_t argb = colour_of(pad);
		xsdk_gpu_resolve(address, width, height, argb);
		if (frame == 1)
			check_first_frame(address, width, height, cleared);
		if (frame == CHECK_FRAMES)
			check_devices(pad, tone, start_ns);
		xsdk_present_end();
		cleared = argb;
	}
	if (pad)
		SDL_CloseGamepad(pad);
}

int main(void)
{
	xsdk_log_write("sdl3: the left stick sets the colour, the right trigger the tone's volume, A rumbles; Back ends");
	const NTSTATUS splash = xsdk_splash_show(SPLASH_MS);
	if (!NT_SUCCESS(splash))
		xsdk_log_write("sdl3: xsdk_splash_show failed: 0x%08X", (unsigned)splash);
	if (!SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
		xsdk_log_write("sdl3: SDL_Init failed: %s", SDL_GetError());
		return 1;
	}
	static Tone tone;
	SDL_AudioStream *const stream = open_tone(&tone);
	const Uint64 start_ns = SDL_GetTicksNS();
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("sdl3: xsdk_gpu_open failed: 0x%08X", (unsigned)status);
		return 1;
	}
	uint32_t width = 0;
	uint32_t height = 0;
	status = xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &width, &height);
	if (!NT_SUCCESS(status)) {
		// The title's exit gives the GPU back.
		xsdk_log_write("sdl3: xsdk_present_open failed: 0x%08X", (unsigned)status);
		return 1;
	}
	run(&tone, start_ns, width, height);
	xsdk_present_close();
	status = xsdk_gpu_close();
	if (!NT_SUCCESS(status))
		xsdk_log_write("sdl3: xsdk_gpu_close failed: 0x%08X", (unsigned)status);
	if (stream)
		SDL_DestroyAudioStream(stream);
	SDL_Quit();
	xsdk_log_write("sdl3: done");
	log_checks();
	return 0;
}

// The checks: what the title can tell of itself from the log, as it can't see the screen or hear the speakers.

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

// Once the GPU has finished the frame, its middle holds the colour cleared before it.
static void check_first_frame(uint32_t address, uint32_t width, uint32_t height, uint32_t argb)
{
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	if (!NT_SUCCESS(status))
		xsdk_log_write("sdl3: xsdk_gpu_wait failed: 0x%08X", (unsigned)status);
	const uint32_t pixel = frame_pixel(address, width, height, width / 2, height / 2);
	xsdk_log_write("sdl3: the first frame's middle held %08X, cleared to %08X", (unsigned)pixel, (unsigned)argb);
	check("sdl3: the first resolved frame holds the colour it was cleared to", NT_SUCCESS(status) && pixel == argb);
}

// A second in: the gamepad is open, as an Xbox 360 controller, and SDL has pulled the tone at 48 kHz, within a tenth.
static void check_devices(SDL_Gamepad *pad, Tone *tone, Uint64 start_ns)
{
	if (pad)
		xsdk_log_write("sdl3: gamepad \"%s\", type %s", SDL_GetGamepadName(pad),
			SDL_GetGamepadStringForType(SDL_GetGamepadType(pad)));
	check("sdl3: a gamepad opened, an Xbox 360 controller", pad && SDL_GetGamepadType(pad) == SDL_GAMEPAD_TYPE_XBOX360);
	const Uint64 frames = (Uint64)SDL_GetAtomicInt(&tone->frames);
	const Uint64 expected = (SDL_GetTicksNS() - start_ns) * TONE_RATE / SDL_NS_PER_SECOND;
	xsdk_log_write("sdl3: SDL pulled %u frames of the tone, %u expected", (unsigned)frames, (unsigned)expected);
	check("sdl3: SDL pulled the tone at 48 kHz", frames >= expected * 9 / 10 && frames <= expected * 11 / 10);
}
