#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"
#include "xsdk/thread.h"

// Milestone 56 step 1's loader probe (docs/scope/56-store-networking.md): launch data, the paths XamLoaderLaunchTitle
// takes, and the command line, between probe_launch.xex and probe_launch_t2.xex, the same title under a second title
// ID, side by side in one folder. A launch ends the title making it, so a run is one case, named in
// probe_launch_start.txt as "<case> [form] [argument]"; a title launched by the probe finds what to do in
// probe_launch_state.txt, written beside it just before the launch. Every run appends what it finds to
// probe_launch_record.txt, as the next run of the same .xex overwrites its log.

enum {
	// The probe's own launch data starts with its magic, then its size and seed, so any run can tell it apart.
	HEADER_BYTES = 16,
	HEADER_SIZE_OFFSET = 8,
	HEADER_SEED_OFFSET = 12,
	SMALL_DATA_BYTES = 64,
	MAX_DATA_BYTES = 65536,
	WORD_BYTES = 32,
	ARGUMENT_BYTES = 128,
	PATH_BYTES = 300,
	LINE_BYTES = 512,
	TEXT_CHARS = 200,
	HEX_BYTES = 32,
	GPU_HOLD_MS = 1000,
};

// A word at the command line's address is read as a pointer only if it falls in the kernel's addresses, where
// ExLoadedImageName's text sits (docs/HARDWARE.md, "Each title's log is named after its .xex").
static const uint32_t KERNEL_LOW = 0x80000000;
static const uint32_t KERNEL_HIGH = 0xA0000000;

static const char START_PATH[] = "probe_launch_start.txt";
static const char STATE_NAME[] = "probe_launch_state.txt";
static const char RECORD_PATH[] = "probe_launch_record.txt";
static const char MAGIC[] = "XSDKLNCH";
enum { MAGIC_BYTES = sizeof MAGIC - 1 };
static const char MISSING_XEX[] = "probe_launch_none.xex";

typedef struct {
	uint32_t hop;
	char case_name[WORD_BYTES];
	char form[WORD_BYTES];
	char argument[ARGUMENT_BYTES];
	// The .xex the state is for: a run of another finds it stale, its launch never having arrived.
	char target[WORD_BYTES * 2];
} State;

typedef struct {
	const char *name;
	// What the launching run does, for the first log line.
	const char *warning;
	void (*run)(const State *state);
} ProbeCase;

static char self[WORD_BYTES * 2];
static char other[WORD_BYTES * 2];
static char record_prefix[WORD_BYTES * 4];
static uint8_t data[MAX_DATA_BYTES];

static int64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static unsigned ms_since(int64_t start_us)
{
	return (unsigned)((now_us() - start_us) / 1000);
}

// Logs the line, and appends it to the record, which outlives the log.
static void __attribute__((format(printf, 1, 2))) record(const char *format, ...)
{
	char line[LINE_BYTES];
	va_list arguments;
	va_start(arguments, format);
	(void)vsnprintf(line, sizeof line, format, arguments);
	va_end(arguments);
	xsdk_log_write("%s", line);
	FILE *const file = fopen(RECORD_PATH, "a");
	if (!file) {
		xsdk_log_write("launch: can't append to %s", RECORD_PATH);
		return;
	}
	fprintf(file, "%s: %s\n", record_prefix, line);
	(void)fclose(file);
}

// The part of path after its last backslash.
static const char *file_part(const char *path)
{
	const char *const slash = strrchr(path, '\\');
	return slash ? slash + 1 : path;
}

static uint8_t pattern(uint32_t seed, uint32_t index)
{
	return (uint8_t)(((seed ^ index) * 2654435761u) >> 24);
}

static void make_data(uint32_t size, uint32_t seed)
{
	for (uint32_t i = 0; i < size; i++)
		data[i] = pattern(seed, i);
	if (size < HEADER_BYTES)
		return;
	memcpy(data, MAGIC, MAGIC_BYTES);
	memcpy(data + HEADER_SIZE_OFFSET, &size, sizeof size);
	memcpy(data + HEADER_SEED_OFFSET, &seed, sizeof seed);
}

static void hex_text(const uint8_t *bytes, uint32_t count, char *text, size_t text_bytes)
{
	text[0] = 0;
	for (uint32_t i = 0; i < count && 3 * i + 4 <= text_bytes; i++)
		snprintf(text + 3 * i, 4, "%02X ", bytes[i]);
}

// Whether data holds the probe's own launch data whole, in words.
static void judge_data(uint32_t size, char *verdict, size_t verdict_bytes)
{
	if (size < HEADER_BYTES || memcmp(data, MAGIC, MAGIC_BYTES) != 0) {
		snprintf(verdict, verdict_bytes, "not the probe's");
		return;
	}
	uint32_t made;
	uint32_t seed;
	memcpy(&made, data + HEADER_SIZE_OFFSET, sizeof made);
	memcpy(&seed, data + HEADER_SEED_OFFSET, sizeof seed);
	uint32_t i = HEADER_BYTES;
	while (i < size && i < made && i < MAX_DATA_BYTES && data[i] == pattern(seed, i))
		i++;
	if (i < size && i < made)
		snprintf(verdict, verdict_bytes, "the probe's, %u bytes made, seed %08X, differing first at byte %u",
			(unsigned)made, (unsigned)seed, (unsigned)i);
	else
		snprintf(verdict, verdict_bytes, "the probe's, %u bytes made, seed %08X, %s", (unsigned)made, (unsigned)seed,
			size == made ? "whole" : "its pattern right but its size not what was made");
}

static void log_launch_data(void)
{
	uint32_t size = 0;
	const uint32_t size_result = XamLoaderGetLaunchDataSize(&size);
	record("launch data: XamLoaderGetLaunchDataSize 0x%08X, %u bytes", (unsigned)size_result, (unsigned)size);
	if (size_result != 0 || size == 0)
		return;
	memset(data, 0, sizeof data);
	const uint32_t got = XamLoaderGetLaunchData(data, size < MAX_DATA_BYTES ? size : MAX_DATA_BYTES);
	char hex[3 * HEX_BYTES + 1];
	hex_text(data, size < HEX_BYTES ? size : HEX_BYTES, hex, sizeof hex);
	char verdict[LINE_BYTES / 2];
	judge_data(size, verdict, sizeof verdict);
	record("launch data: XamLoaderGetLaunchData 0x%08X: %s; %s", (unsigned)got, hex, verdict);
	uint32_t after = 0;
	const uint32_t after_result = XamLoaderGetLaunchDataSize(&after);
	record("launch data: after reading it, XamLoaderGetLaunchDataSize 0x%08X, %u bytes", (unsigned)after_result,
		(unsigned)after);
}

// The text at text, escaped, and its first bytes.
static void log_text(const char *what, const char *text)
{
	char shown[TEXT_CHARS * 4 + 1];
	size_t length = 0;
	for (uint32_t i = 0; i < TEXT_CHARS && text[i]; i++) {
		const unsigned char c = (unsigned char)text[i];
		length += (size_t)snprintf(shown + length, sizeof shown - length, c >= 0x20 && c < 0x7F ? "%c" : "\\x%02X", c);
	}
	shown[length] = 0;
	char hex[3 * HEX_BYTES + 1];
	hex_text((const uint8_t *)text, HEX_BYTES, hex, sizeof hex);
	record("%s: \"%s\"; first bytes %s", what, shown, hex);
}

static void log_command_line(void)
{
	record("ExLoadedImageName: %s", ExLoadedImageName ? ExLoadedImageName : "(none)");
	const char *const line = ExLoadedCommandLine;
	record("ExLoadedCommandLine at %p", (const void *)line);
	if (!line)
		return;
	log_text("ExLoadedCommandLine", line);
	uint32_t first;
	memcpy(&first, line, sizeof first);
	if (first >= KERNEL_LOW && first < KERNEL_HIGH)
		log_text("ExLoadedCommandLine's first word, read as a pointer", (const char *)(uintptr_t)first);
}

// The state file beside the .xex at launch_path: game:'s, or a device path's own folder.
static void state_path(const char *launch_path, char *path, size_t path_bytes)
{
	if (launch_path[0] != '\\') {
		snprintf(path, path_bytes, "%s", STATE_NAME);
		return;
	}
	const int folder_chars = (int)(file_part(launch_path) - launch_path);
	snprintf(path, path_bytes, "%.*s%s", folder_chars, launch_path, STATE_NAME);
}

static bool write_state(const State *state, const char *launch_path)
{
	char path[PATH_BYTES];
	state_path(launch_path, path, sizeof path);
	FILE *const file = fopen(path, "w");
	if (!file) {
		record("can't write %s", path);
		return false;
	}
	fprintf(file, "%u %s %s %s %s\n", (unsigned)state->hop, state->case_name, state->form, state->target,
		state->argument[0] ? state->argument : "-");
	return fclose(file) == 0;
}

// The state a launch left for this run, taken away so it's used once; false if there's none or it's for another .xex,
// which note says.
static bool read_state(State *state, char *note, size_t note_bytes)
{
	FILE *const file = fopen(STATE_NAME, "r");
	if (!file)
		return false;
	unsigned hop = 0;
	const int read =
		fscanf(file, "%u %31s %31s %63s %127s", &hop, state->case_name, state->form, state->target, state->argument);
	(void)fclose(file);
	(void)unlink(STATE_NAME);
	if (read != 5) {
		snprintf(note, note_bytes, "%s didn't read as a state", STATE_NAME);
		return false;
	}
	state->hop = hop;
	if (strcmp(state->argument, "-") == 0)
		state->argument[0] = 0;
	// FATX's names ignore case.
	if (strcasecmp(state->target, self) != 0) {
		snprintf(note, note_bytes, "a stale state: hop %u of %s, by %s, was for %s, which never arrived", (unsigned)hop,
			state->case_name, state->form, state->target);
		return false;
	}
	return true;
}

static void read_start(State *state)
{
	memset(state, 0, sizeof *state);
	strcpy(state->case_name, "look");
	strcpy(state->form, "device");
	FILE *const file = fopen(START_PATH, "r");
	if (!file)
		return;
	char form[WORD_BYTES] = "";
	if (fscanf(file, "%31s %31s %127s", state->case_name, form, state->argument) >= 2)
		strcpy(state->form, form);
	(void)fclose(file);
}

// Where a launch by form goes, and the .xex that should arrive.
static bool launch_path(const char *form, const char *argument, char *path, size_t path_bytes, const char **target)
{
	*target = other;
	if (strcmp(form, "game") == 0)
		snprintf(path, path_bytes, "game:\\%s", other);
	else if (strcmp(form, "bare") == 0)
		snprintf(path, path_bytes, "%s", other);
	else if (strcmp(form, "device") == 0)
		snprintf(path, path_bytes, "%.*s%s", (int)(file_part(ExLoadedImageName) - ExLoadedImageName), ExLoadedImageName,
			other);
	else if (strcmp(form, "usb") == 0) {
		snprintf(path, path_bytes, "\\Device\\Mass0\\%s", argument[0] ? argument : other);
		*target = file_part(path);
	} else if (strcmp(form, "missing") == 0) {
		snprintf(path, path_bytes, "game:\\%s", MISSING_XEX);
		*target = MISSING_XEX;
	} else {
		record("no launch form '%s': game, bare, device, usb or missing", form);
		return false;
	}
	return true;
}

// Writes the next hop's state beside what it launches, then launches it; returns only if XamLoaderLaunchTitle does.
static void launch(const State *state, uint32_t next_hop)
{
	char path[PATH_BYTES];
	const char *target;
	if (!launch_path(state->form, state->argument, path, sizeof path, &target))
		return;
	State next = *state;
	next.hop = next_hop;
	snprintf(next.target, sizeof next.target, "%.63s", target);
	if (!write_state(&next, path))
		return;
	record("launching %s, by %s, for hop %u", path, state->form, (unsigned)next_hop);
	xsdk_log_write("launch: nothing more here unless XamLoaderLaunchTitle returns; %s has the rest", RECORD_PATH);
	xsdk_log_close();
	const int64_t start = now_us();
	XamLoaderLaunchTitle(path, 0);
	record("XamLoaderLaunchTitle(%s) returned after %u ms", path, ms_since(start));
	char state_file[PATH_BYTES];
	state_path(path, state_file, sizeof state_file);
	(void)unlink(state_file);
}

static void set_data(uint32_t size)
{
	make_data(size, (uint32_t)now_us());
	const uint32_t result = XamLoaderSetLaunchData(data, size);
	record("XamLoaderSetLaunchData, %u bytes: 0x%08X", (unsigned)size, (unsigned)result);
}

// Steps XamLoaderSetLaunchData's size up, then narrows down to the most it takes; returns that.
static uint32_t largest_data(void)
{
	static const uint32_t sizes[] = { 1, 64, 256, 512, 1000, 1024, 2048, 4096, 8192, 16384, MAX_DATA_BYTES };
	uint32_t taken = 0;
	uint32_t refused = 0;
	for (uint32_t i = 0; i < sizeof sizes / sizeof sizes[0] && !refused; i++) {
		make_data(sizes[i], i);
		const uint32_t result = XamLoaderSetLaunchData(data, sizes[i]);
		uint32_t held = 0;
		const uint32_t held_result = XamLoaderGetLaunchDataSize(&held);
		record(
			"sizes: XamLoaderSetLaunchData, %u bytes: 0x%08X; this title's XamLoaderGetLaunchDataSize then 0x%08X, "
			"%u bytes",
			(unsigned)sizes[i], (unsigned)result, (unsigned)held_result, (unsigned)held);
		if (result == 0)
			taken = sizes[i];
		else
			refused = sizes[i];
	}
	while (refused && refused - taken > 1) {
		const uint32_t middle = taken + (refused - taken) / 2;
		make_data(middle, middle);
		if (XamLoaderSetLaunchData(data, middle) == 0)
			taken = middle;
		else
			refused = middle;
	}
	if (refused)
		record("sizes: XamLoaderSetLaunchData takes %u bytes at most, refusing %u", (unsigned)taken, (unsigned)refused);
	else
		record("sizes: XamLoaderSetLaunchData took every size up to the probe's %u bytes", (unsigned)taken);
	return taken;
}

static void run_look(const State *state)
{
	(void)state;
}

// Hop 0 sets the most data it can and launches the other title, which launches this one back without setting any.
static void run_data(const State *state)
{
	if (state->hop == 0) {
		unsigned asked = 0;
		uint32_t size = largest_data();
		if (sscanf(state->argument, "%u", &asked) == 1 && asked > 0 && asked <= MAX_DATA_BYTES)
			size = asked;
		set_data(size);
		launch(state, 1);
	} else if (state->hop == 1)
		launch(state, 2);
	else
		record("data: back after two launches; the launch data above is what's left of the first title's");
}

static void run_path(const State *state)
{
	if (state->hop > 0) {
		record("path: arrived by %s", state->form);
		return;
	}
	set_data(SMALL_DATA_BYTES);
	launch(state, 1);
}

// Shows the logo, so the GPU and the display are the title's, then launches with them held or handed back first.
static void launch_after_gpu(const State *state, bool hand_back)
{
	if (state->hop > 0) {
		record("gpu: arrived, launched with the GPU %s", hand_back ? "handed back" : "held");
		return;
	}
	NTSTATUS status = xsdk_gpu_open();
	record("gpu: xsdk_gpu_open 0x%08X", (unsigned)status);
	if (!NT_SUCCESS(status))
		return;
	status = xsdk_splash_begin();
	record("gpu: xsdk_splash_begin 0x%08X", (unsigned)status);
	(void)xsdk_thread_sleep(GPU_HOLD_MS);
	if (hand_back) {
		xsdk_splash_end();
		record("gpu: handed back: xsdk_gpu_close 0x%08X", (unsigned)xsdk_gpu_close());
	}
	set_data(SMALL_DATA_BYTES);
	launch(state, 1);
}

static void run_gpu_held(const State *state)
{
	launch_after_gpu(state, false);
}

static void run_gpu_closed(const State *state)
{
	launch_after_gpu(state, true);
}

static const ProbeCase cases[] = {
	{ "look", "only looks, then exits to the dashboard", run_look },
	{ "data",
		"steps XamLoaderSetLaunchData up to 64 KB, then ends by launching the other title, which launches this "
		"one back",
		run_data },
	{ "path",
		"ends by launching the other title; a path that doesn't work may return, hang the console or go to the "
		"dashboard",
		run_path },
	{ "gpu_held", "launches with the GPU and the display still the title's, which may hang the console", run_gpu_held },
	{ "gpu_closed", "hands the GPU and the display back, then ends by launching the other title", run_gpu_closed },
};

int main(void)
{
	const char *const image = ExLoadedImageName ? ExLoadedImageName : "";
	snprintf(self, sizeof self, "%s", file_part(image));
	snprintf(other, sizeof other, "%s",
		strcasecmp(self, "probe_launch_t2.xex") == 0 ? "probe_launch.xex" : "probe_launch_t2.xex");
	State state;
	char note[LINE_BYTES / 2] = "";
	if (!read_state(&state, note, sizeof note))
		read_start(&state);
	const ProbeCase *chosen = NULL;
	for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		if (strcmp(state.case_name, cases[i].name) == 0)
			chosen = &cases[i];
	}
	struct timespec now;
	(void)clock_gettime(CLOCK_REALTIME, &now);
	const struct tm *const utc = gmtime(&now.tv_sec);
	snprintf(record_prefix, sizeof record_prefix, "%02d:%02d:%02d %s, hop %u of %s", utc->tm_hour, utc->tm_min,
		utc->tm_sec, self, (unsigned)state.hop, state.case_name);
	if (!chosen) {
		xsdk_log_write("probe_launch: no case '%s': look, data, path, gpu_held or gpu_closed", state.case_name);
		return 1;
	}
	const char *what = "launched by the probe; logs what arrived";
	if (state.hop == 0)
		what = chosen->warning;
	else if (chosen->run == run_data && state.hop == 1)
		what = "launched by the probe; logs what arrived, then ends by launching the other title back";
	record("probe_launch: %s, hop %u of %s, by %s %s: %s", self, (unsigned)state.hop, chosen->name, state.form,
		state.argument, what);
	if (note[0])
		record("%s", note);
	log_command_line();
	log_launch_data();
	chosen->run(&state);
	record("probe_launch: hop %u of %s done; exiting to the dashboard", (unsigned)state.hop, chosen->name);
	return 0;
}
