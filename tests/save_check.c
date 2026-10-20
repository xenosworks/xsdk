#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bar_scene.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/splash.h"
#include "xsdk/xam.h"

// Milestone 51's check with a person at the console and a profile signed in (docs/scope/51-saves-and-profiles.md).
// The first launch reads the gamertag, saves to a package, then opens the keyboard and a message box once; a marker
// beside the log then says the next launch, after a reboot, reads the save back and deletes it. The bar moves all
// along, as XAM's dialogs show only while the title presents, and the title ends only on Back with XAM's UI closed.

enum {
	SPLASH_MS = 2000,
	BUFFERS = 3,
	SWAP_INTERVAL = 1,
	BAR_ARGB = 0xFF40C040,
	// The Guide's first opening held a frame up 2.7 s while XAM loaded it (docs/HARDWARE.md).
	BEGIN_TIMEOUT_MS = 5000,
	MAX_MISSES_LOGGED = 4,
	TEXT_BYTES = 128,
	PATH_BYTES = 256,
	KEYBOARD_CHARS = 32,
	BUTTONS = 2,
};

typedef enum {
	MOUNTING,
	UNMOUNTING,
	DELETING,
	KEYBOARD,
	// The keyboard done, waiting for XAM's UI to close before the message box opens.
	BETWEEN_DIALOGS,
	MESSAGE_BOX,
	DONE,
} Step;

static const char *const save_file = "save:/save_check.dat";
static const uint16_t keyboard_title[] = u"save_check: the keyboard";
static const uint16_t keyboard_description[] = u"Type a word and accept it. The log says what came back.";
static const uint16_t keyboard_default[] = u"xsdk";
static const uint16_t box_title[] = u"save_check: a message box";
static const uint16_t box_text[] = u"Press either button. The log says which.";
static const uint16_t first_button[] = u"First";
static const uint16_t second_button[] = u"Second";
static const uint16_t *const buttons[BUTTONS] = { first_button, second_button };

static uint32_t passed_count;
static uint32_t failed_count;
static uint32_t frame;
static bool reading_back;
static char marker_path[PATH_BYTES];
// What the first launch writes, and what the next reads back.
static char text[TEXT_BYTES];
static uint32_t user = XSDK_XAM_USER_NONE;
static char gamertag[XSDK_XAM_GAMERTAG_BYTES];
static Step step;
// XAM writes into these after the call that starts them returns.
static XsdkSave save;
static XAM_OVERLAPPED dialog;
static uint16_t typed[KEYBOARD_CHARS];
static uint32_t pressed;
static bool ui_seen;

static void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	passed_count += passed;
	failed_count += !passed;
}

// The log's name with .saved for .log, so each .xex built from this has a marker of its own.
static void name_marker(void)
{
	const char *const log = xsdk_log_path();
	const size_t length = log ? strlen(log) : 0;
	if (length > 4 && length + 2 < sizeof marker_path)
		snprintf(marker_path, sizeof marker_path, "%.*s.saved", (int)(length - 4), log);
	else
		strcpy(marker_path, "game:\\save_check.saved");
}

static bool read_text(const char *path, char *read, size_t read_bytes)
{
	FILE *const file = fopen(path, "rb");
	if (!file)
		return false;
	const size_t length = fread(read, 1, read_bytes - 1, file);
	read[length] = '\0';
	return fclose(file) == 0 && length > 0;
}

static bool write_text(const char *path, const char *written)
{
	FILE *const file = fopen(path, "wb");
	if (!file)
		return false;
	const bool wrote = fwrite(written, 1, strlen(written), file) == strlen(written);
	return fclose(file) == 0 && wrote;
}

// UTF-16 as ASCII, '?' for anything else.
static void ascii_of(const uint16_t *chars, uint32_t count, char *ascii, size_t ascii_bytes)
{
	size_t used = 0;
	for (uint32_t i = 0; i < count && chars[i] && used + 1 < ascii_bytes; i++)
		ascii[used++] = chars[i] >= 0x20 && chars[i] < 0x7F ? (char)chars[i] : '?';
	ascii[used] = '\0';
}

// The signed-in user, or user 0 with nobody signed in.
static uint32_t dialog_user(void)
{
	return user == XSDK_XAM_USER_NONE ? 0 : user;
}

static void start_message_box(void)
{
	pressed = UINT32_MAX;
	ui_seen = false;
	const uint32_t returned =
		xsdk_xam_message_box(dialog_user(), box_title, box_text, buttons, BUTTONS, &pressed, &dialog);
	xsdk_log_write("save_check: xsdk_xam_message_box returned 0x%08X at frame %u", (unsigned)returned, (unsigned)frame);
	step = MESSAGE_BOX;
	if (returned != ERROR_IO_PENDING) {
		check("save_check: the message box closed on a button", false);
		step = DONE;
	}
}

static void start_keyboard(void)
{
	memset(typed, 0, sizeof typed);
	ui_seen = false;
	const uint32_t returned = xsdk_xam_keyboard(
		dialog_user(), keyboard_title, keyboard_description, keyboard_default, typed, KEYBOARD_CHARS, &dialog);
	xsdk_log_write("save_check: xsdk_xam_keyboard returned 0x%08X at frame %u", (unsigned)returned, (unsigned)frame);
	step = KEYBOARD;
	if (returned != ERROR_IO_PENDING) {
		check("save_check: the keyboard closed and its text was read", false);
		step = BETWEEN_DIALOGS;
	}
}

// What a save call returned, ERROR_IO_PENDING moving on to next; anything else is its result, handled at once.
static void wait_for(Step next, uint32_t returned, void (*done)(uint32_t result))
{
	step = next;
	if (returned != ERROR_IO_PENDING)
		done(returned);
}

static void deleted(uint32_t result)
{
	xsdk_log_write("save_check: XamContentDelete 0x%08X", (unsigned)result);
	const bool marker_gone = remove(marker_path) == 0;
	check("save_check: the save and the marker deleted", result == ERROR_SUCCESS && marker_gone);
	step = DONE;
}

static void unmounted(uint32_t result)
{
	xsdk_log_write("save_check: unmounted 0x%08X, the flush 0x%08X", (unsigned)result, (unsigned)save.flush_result);
	check("save_check: the save flushed and unmounted", result == ERROR_SUCCESS && save.flush_result == ERROR_SUCCESS);
	if (reading_back) {
		xsdk_xam_prepare(&save.overlapped);
		const uint32_t returned = XamContentDelete(save.user_index, &save.data, &save.overlapped);
		wait_for(DELETING, xsdk_xam_returned(&save.overlapped, returned), deleted);
		return;
	}
	check(
		"save_check: the marker written for the next launch", result == ERROR_SUCCESS && write_text(marker_path, text));
	start_keyboard();
}

static void mounted(uint32_t result)
{
	xsdk_log_write("save_check: mounted 0x%08X, disposition %u, at frame %u", (unsigned)result,
		(unsigned)save.disposition, (unsigned)frame);
	if (reading_back) {
		check("save_check: the last launch's save is there",
			result == ERROR_SUCCESS && save.disposition == XSDK_SAVE_OPENED);
		char read[TEXT_BYTES] = "";
		const bool read_it = result == ERROR_SUCCESS && read_text(save_file, read, sizeof read);
		xsdk_log_write("save_check: read back '%s', expected '%s'", read, text);
		check("save_check: it reads back as the last launch wrote it", read_it && strcmp(read, text) == 0);
	} else {
		// A package already there means a first launch that never got to the second, or saves shared between titles.
		check("save_check: the save was made new", result == ERROR_SUCCESS && save.disposition == XSDK_SAVE_CREATED);
		check("save_check: the save written", result == ERROR_SUCCESS && write_text(save_file, text));
	}
	if (result == ERROR_SUCCESS)
		wait_for(UNMOUNTING, xsdk_save_unmount(&save), unmounted);
	else if (reading_back)
		step = DONE;
	else
		start_keyboard();
}

static void dialog_done(uint32_t result)
{
	if (step == KEYBOARD) {
		char ascii[KEYBOARD_CHARS + 1];
		ascii_of(typed, KEYBOARD_CHARS, ascii, sizeof ascii);
		xsdk_log_write(
			"save_check: the keyboard gave 0x%08X, extended error 0x%08X, length 0x%08X, text '%s', at frame %u",
			(unsigned)result, (unsigned)dialog.extended_error, (unsigned)dialog.length, ascii, (unsigned)frame);
		check("save_check: the keyboard closed and its text was read", result == ERROR_SUCCESS);
		check("save_check: XAM's UI showed open while the keyboard was", ui_seen);
		step = BETWEEN_DIALOGS;
		return;
	}
	xsdk_log_write("save_check: the message box gave 0x%08X, extended error 0x%08X, button %u, at frame %u",
		(unsigned)result, (unsigned)dialog.extended_error, (unsigned)pressed, (unsigned)frame);
	check("save_check: the message box closed on a button", result == ERROR_SUCCESS && pressed < BUTTONS);
	check("save_check: XAM's UI showed open while the message box was", ui_seen);
	step = DONE;
}

// One look a frame at whatever runs, never waiting.
static void advance(bool ui_open)
{
	if (step == BETWEEN_DIALOGS && !ui_open)
		start_message_box();
	uint32_t result = ERROR_IO_INCOMPLETE;
	if (step == MOUNTING || step == UNMOUNTING || step == DELETING)
		result = xsdk_save_wait(&save, 0);
	else if (step == KEYBOARD || step == MESSAGE_BOX)
		result = xsdk_xam_wait(&dialog, 0, NULL);
	if (result == ERROR_IO_INCOMPLETE)
		return;
	if (step == MOUNTING)
		mounted(result);
	else if (step == UNMOUNTING)
		unmounted(result);
	else if (step == DELETING)
		deleted(result);
	else
		dialog_done(result);
}

static bool back_pressed(void)
{
	for (uint32_t pad = 0; pad < XSDK_XAM_USERS; pad++) {
		INPUT_STATE input;
		if (XamInputGetState(pad, 0, &input) == ERROR_SUCCESS && input.gamepad.buttons & INPUT_GAMEPAD_BACK)
			return true;
	}
	return false;
}

static void start(void)
{
	name_marker();
	reading_back = read_text(marker_path, text, sizeof text);
	const uint32_t result = xsdk_xam_signed_in_user(&user, gamertag);
	xsdk_log_write("save_check: user 0x%X, gamertag '%s', 0x%08X; %s, marker %s", (unsigned)user, gamertag,
		(unsigned)result, reading_back ? "reading back the last launch's save" : "saving", marker_path);
	check("save_check: a profile is signed in and its gamertag read", result == ERROR_SUCCESS && gamertag[0]);
	if (!reading_back) {
		uint64_t now = 0;
		KeQuerySystemTime(&now);
		snprintf(text, sizeof text, "save_check for '%s' at %llu", gamertag, (unsigned long long)now);
	}
	wait_for(MOUNTING, xsdk_save_mount(&save, user, "save", "save_check", u"xsdk save_check"), mounted);
}

int main(void)
{
	xsdk_log_write("save_check: answer the keyboard and the message box when they show, then press Back");
	(void)xsdk_splash_show(SPLASH_MS);
	const NTSTATUS status = xsdk_gpu_open();
	BarScene scene;
	const bool opened = NT_SUCCESS(status) && bar_scene_open(&scene, BAR_ARGB);
	uint32_t width = 0;
	uint32_t height = 0;
	const bool presenting =
		opened && NT_SUCCESS(xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &width, &height));
	check("save_check: GPU, scene and present open", presenting);
	if (!presenting)
		return 1;
	bool ui_open = xsdk_system_ui_open();
	start();
	uint32_t misses = 0;
	bool told_done = false;
	const char *const next_launch =
		reading_back ? "and the next launch saves afresh" : "then reboot and launch again to read the save back";
	for (;; frame++) {
		uint32_t address;
		if (NT_SUCCESS(xsdk_present_begin(&address, BEGIN_TIMEOUT_MS))) {
			bar_scene_draw(&scene, frame, address);
			xsdk_present_end();
		} else if (misses++ < MAX_MISSES_LOGGED) {
			xsdk_log_write("save_check: no front buffer came free in %u ms at frame %u", (unsigned)BEGIN_TIMEOUT_MS,
				(unsigned)frame);
		}
		if (xsdk_system_ui_open() != ui_open) {
			ui_open = !ui_open;
			xsdk_log_write("save_check: XAM's UI %s at frame %u", ui_open ? "opened" : "closed", (unsigned)frame);
		}
		ui_seen |= ui_open;
		advance(ui_open);
		if (step == DONE && !told_done)
			xsdk_log_write("save_check: done at frame %u; press Back, %s", (unsigned)frame, next_launch);
		told_done |= step == DONE;
		if (step == DONE && !ui_open && back_pressed())
			break;
	}
	xsdk_present_close();
	bar_scene_close(&scene);
	check("save_check: every frame queued in time", misses == 0);
	check("save_check: GPU close", NT_SUCCESS(xsdk_gpu_close()));
	xsdk_log_write("save_check: %u passed, %u failed", (unsigned)passed_count, (unsigned)failed_count);
	return failed_count != 0;
}
