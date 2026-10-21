#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/xam.h"

// Milestone 56's launch check (docs/scope/56-store-networking.md, Verification), which the console agent runs with
// nobody there. It launches launch_target.xex beside it, then a copy of it outside its folder, each time with its own
// path and data, and launch_target launches it back with the data reversed. A launch ends the title, so
// launch_check_state.txt keeps the stage and the checks so far, and each start logs them again: the log after the
// last start holds every check, and ends "launch_check: ... done".

enum {
	STAGE_BESIDE = 1,
	STAGE_OUTSIDE = 2,
	SMALL_DATA_BYTES = 64,
	// A launch back comes within seconds; an older state is from a run that never came back.
	RETURN_WINDOW_S = 20,
	MAX_LINES = 16,
	LINE_BYTES = 192,
	PATH_BYTES = 260,
	COPY_BYTES = 65536,
};

static const char STATE_PATH[] = "launch_check_state.txt";
static const char TARGET[] = "launch_target.xex";
static const char MISSING[] = "launch_none.xex";
// On the hard disk, outside the folder the agent runs titles from.
static const char OUTSIDE_FOLDER[] = "hdd:/xsdk_launch_outside";
static const char OUTSIDE_TARGET[] = "hdd:/xsdk_launch_outside/launch_target.xex";

typedef struct {
	int stage;
	long long launched_at;
	// The path sent to launch back to: launch_check's own, as the loader gave it at the first start.
	char path[PATH_BYTES];
	int line_count;
	char lines[MAX_LINES][LINE_BYTES];
} State;

static State state;
static uint8_t sent[XSDK_LAUNCH_DATA_MAX_BYTES + 1];
static uint8_t back[XSDK_LAUNCH_DATA_MAX_BYTES];
static uint8_t copy_buffer[COPY_BYTES];

static void check(const char *name, bool passed)
{
	if (state.line_count == MAX_LINES)
		return;
	char *const line = state.lines[state.line_count++];
	snprintf(line, LINE_BYTES, "%s: %s", passed ? "PASS" : "FAIL", name);
	xsdk_log_write("%s", line);
}

static void strip_newline(char *line)
{
	line[strcspn(line, "\r\n")] = '\0';
}

// The state a launch left, taken away so it's used once, its checks logged again; false if there's none, or it's
// older than a launch back takes.
static bool read_state(void)
{
	FILE *const file = fopen(STATE_PATH, "r");
	if (!file)
		return false;
	const bool read =
		fscanf(file, "%d %lld ", &state.stage, &state.launched_at) == 2 && fgets(state.path, PATH_BYTES, file);
	strip_newline(state.path);
	while (read && state.line_count < MAX_LINES && fgets(state.lines[state.line_count], LINE_BYTES, file))
		strip_newline(state.lines[state.line_count++]);
	(void)fclose(file);
	(void)unlink(STATE_PATH);
	const long long age_s = (long long)time(NULL) - state.launched_at;
	if (!read || age_s < 0 || age_s > RETURN_WINDOW_S) {
		xsdk_log_write(
			"launch_check: %s is from a run %lld s ago that never came back; starting afresh", STATE_PATH, age_s);
		memset(&state, 0, sizeof(state));
		return false;
	}
	for (int i = 0; i < state.line_count; i++)
		xsdk_log_write("%s", state.lines[i]);
	return true;
}

static bool write_state(int stage)
{
	FILE *const file = fopen(STATE_PATH, "w");
	if (!file) {
		xsdk_log_write("launch_check: can't write %s: errno %d", STATE_PATH, errno);
		return false;
	}
	state.stage = stage;
	state.launched_at = (long long)time(NULL);
	fprintf(file, "%d %lld\n%s\n", state.stage, state.launched_at, state.path);
	for (int i = 0; i < state.line_count; i++)
		fprintf(file, "%s\n", state.lines[i]);
	return fclose(file) == 0;
}

// The path to launch back to, then bytes of the stage's own.
static void make_data(int stage, size_t size_bytes)
{
	const size_t path_bytes = strlen(state.path) + 1;
	memcpy(sent, state.path, path_bytes);
	for (size_t i = path_bytes; i < size_bytes; i++)
		sent[i] = (uint8_t)(i * 7 + (size_t)stage * 101 + 1);
}

// Whether the data came back reversed: what was sent, or the start of it, read backwards.
static bool came_back_reversed(size_t back_bytes)
{
	for (size_t i = 0; i < back_bytes; i++) {
		if (back[back_bytes - 1 - i] != sent[i])
			return false;
	}
	return back_bytes > 0;
}

// Ends the title, launching path with the stage's data, which comes back with the launch back; returns if it can't.
static void launch(int stage, const char *path, size_t size_bytes)
{
	make_data(stage, size_bytes);
	if (!write_state(stage))
		return;
	xsdk_log_write(
		"launch_check: launching %s with %u bytes; it launches launch_check back", path, (unsigned)size_bytes);
	(void)xsdk_launch(path, sent, size_bytes);
	const int error = errno;
	(void)unlink(STATE_PATH);
	char name[LINE_BYTES];
	snprintf(name, sizeof(name), "launch: launching %s, which failed with errno %d", path, error);
	check(name, false);
}

static void check_start(int argc, char **argv)
{
	const char *const image = ExLoadedImageName ? ExLoadedImageName : "";
	const char *const slash = strrchr(image, '\\');
	const char *const own_name = slash ? slash + 1 : image;
	xsdk_log_write("launch_check: argc %d, argv[0] \"%s\"", argc, argv[0]);
	check("launch: argv holds the .xex's name alone, as the loader's command line does",
		argc == 1 && strcasecmp(argv[0], own_name) == 0);

	// Both fail before anything ends; XAM ends the title for a launch to nothing (docs/HARDWARE.md).
	(void)xsdk_launch(TARGET, sent, XSDK_LAUNCH_DATA_MAX_BYTES + 1);
	const int too_big = errno;
	xsdk_log_write(
		"launch_check: 1021 bytes for %s gave errno %d; now %s, which isn't there", TARGET, too_big, MISSING);
	(void)xsdk_launch(MISSING, sent, SMALL_DATA_BYTES);
	const int missing = errno;
	check("launch: a .xex that isn't there fails with ENOENT and 1021 bytes of data with E2BIG, the title running on",
		missing == ENOENT && too_big == E2BIG);
}

static void check_beside(size_t back_bytes)
{
	make_data(STAGE_BESIDE, XSDK_LAUNCH_DATA_MAX_BYTES);
	const bool reversed = came_back_reversed(back_bytes);
	const bool as_sent = back_bytes == XSDK_LAUNCH_DATA_MAX_BYTES && memcmp(back, sent, back_bytes) == 0;
	check("launch: the round trip through launch_target beside it brings the data back reversed", reversed);
	check("launch: 1020 bytes of data, the most XAM passes on, arrive whole",
		back_bytes == XSDK_LAUNCH_DATA_MAX_BYTES && (reversed || as_sent));
}

// Copies launch_target.xex outside the title's folder, where a launch goes through xsdk's link.
static bool copy_target(void)
{
	if (mkdir(OUTSIDE_FOLDER, 0777) != 0 && errno != EEXIST) {
		xsdk_log_write("launch_check: mkdir(%s) failed: errno %d", OUTSIDE_FOLDER, errno);
		return false;
	}
	FILE *const from = fopen(TARGET, "rb");
	FILE *const to = from ? fopen(OUTSIDE_TARGET, "wb") : NULL;
	bool copied = to != NULL;
	size_t read_bytes = 0;
	while (copied && (read_bytes = fread(copy_buffer, 1, sizeof(copy_buffer), from)) > 0)
		copied = fwrite(copy_buffer, 1, read_bytes, to) == read_bytes;
	copied = copied && !ferror(from);
	if (to && fclose(to) != 0)
		copied = false;
	if (from)
		(void)fclose(from);
	if (!copied)
		xsdk_log_write("launch_check: copying %s to %s failed: errno %d", TARGET, OUTSIDE_TARGET, errno);
	return copied;
}

static int finish(void)
{
	int failed = 0;
	for (int i = 0; i < state.line_count; i++)
		failed += strncmp(state.lines[i], "FAIL", 4) == 0;
	xsdk_log_write("launch_check: %d checks, %d failed; done", state.line_count, failed);
	return failed != 0;
}

int main(int argc, char **argv)
{
	xsdk_log_write("launch_check: launches launch_target.xex beside it, then outside its folder, each launching back");
	const size_t back_bytes = xsdk_launch_data(back, sizeof(back));
	if (!read_state()) {
		xsdk_log_write("launch_check: a first start, %u bytes of launch data", (unsigned)back_bytes);
		snprintf(state.path, sizeof(state.path), "%s", ExLoadedImageName ? ExLoadedImageName : "launch_check.xex");
		check_start(argc, argv);
		launch(STAGE_BESIDE, TARGET, XSDK_LAUNCH_DATA_MAX_BYTES);
		return finish();
	}
	xsdk_log_write("launch_check: back from stage %d with %u bytes of launch data", state.stage, (unsigned)back_bytes);
	if (state.stage == STAGE_BESIDE) {
		check_beside(back_bytes);
		if (copy_target())
			launch(STAGE_OUTSIDE, OUTSIDE_TARGET, SMALL_DATA_BYTES);
		else
			check("launch: launch_target.xex copied outside the title's folder", false);
		return finish();
	}
	make_data(STAGE_OUTSIDE, SMALL_DATA_BYTES);
	check(
		"launch: launch_target outside the title's folder, launched through xsdk's link, launches back with the "
		"data reversed",
		state.stage == STAGE_OUTSIDE && back_bytes == SMALL_DATA_BYTES && came_back_reversed(back_bytes));
	return finish();
}
