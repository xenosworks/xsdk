#ifndef XSDK_XAM_H
#define XSDK_XAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xsdk/kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

// XAM's asynchronous calls, saves, the signed-in user and XAM's dialogs: docs/GUIDE.md, "Saves, the user and XAM's
// dialogs". Launching another .xex: docs/GUIDE.md, "Launching".

enum {
	XSDK_XAM_USERS = 4,
	// A save no profile owns, which content takes with nobody signed in (results/2026-10-19-milestone-51/
	// probe_xam.log), as XAM's internal content calls use it (xenia:src/xenia/kernel/xam/xam_content.cc:245-254).
	XSDK_XAM_USER_NONE = 0xFE,
	// A gamertag's bytes, its terminating zero included (xenia:src/xenia/kernel/xam/xam_user.cc:105-120).
	XSDK_XAM_GAMERTAG_BYTES = 16,
	// A save's drive name, its terminating zero included, as "save" for save:.
	XSDK_SAVE_DRIVE_BYTES = 16,
	// XamContentCreateEx's dispositions (xenia:src/xenia/kernel/xam/xam_content.cc:119).
	XSDK_SAVE_CREATED = 1,
	XSDK_SAVE_OPENED = 2,
	// The most launch data that reaches the launched title (docs/HARDWARE.md, "XAM's sockets and loader, milestone
	// 56").
	XSDK_LAUNCH_DATA_MAX_BYTES = 1020,
};

// Readies an overlapped not in use for one call: zeroed, with an event of its own that xsdk_xam_wait closes once the
// call is done. Without an event, which only a failed NtCreateEvent leaves, xsdk_xam_wait can only look.
void xsdk_xam_prepare(XAM_OVERLAPPED *overlapped);
// Takes what the call returned: ERROR_IO_PENDING while XAM runs it, else its result, the event closed.
uint32_t xsdk_xam_returned(XAM_OVERLAPPED *overlapped, uint32_t returned);
// For a call that returned ERROR_IO_PENDING: ERROR_IO_INCOMPLETE until it's done, waiting up to timeout_ms for its
// event (0 only looks); then XAM's result, with its length if length isn't NULL. How it went also shows in the
// overlapped's extended_error, which a cancelled keyboard may set.
uint32_t xsdk_xam_wait(XAM_OVERLAPPED *overlapped, uint32_t timeout_ms, uint32_t *length);

// The first user signed in and their gamertag: XamUserGetName's result, or ERROR_NO_SUCH_USER with nobody signed in,
// user_index then XSDK_XAM_USER_NONE.
uint32_t xsdk_xam_signed_in_user(uint32_t *user_index, char gamertag[XSDK_XAM_GAMERTAG_BYTES]);

// Opens the on-screen keyboard; the text, UTF-16, lands in text once the call is done.
uint32_t xsdk_xam_keyboard(uint32_t user_index, const uint16_t *title, const uint16_t *description,
	const uint16_t *default_text, uint16_t *text, uint32_t text_chars, XAM_OVERLAPPED *overlapped);
// Opens a message box; the button pressed, from 0, lands in pressed once the call is done.
uint32_t xsdk_xam_message_box(uint32_t user_index, const uint16_t *title, const uint16_t *text,
	const uint16_t *const *buttons, uint32_t button_count, uint32_t *pressed, XAM_OVERLAPPED *overlapped);

// Whether the Guide or a dialog of XAM's is open, from XAM's notifications; call it once a frame from one thread. The
// first call starts listening, and the dialogs above call it, so call it once at start if the Guide matters before.
bool xsdk_system_ui_open(void);

// A save game: a content package on the hard disk mounted as a drive. XAM writes into it while a call on it runs, so
// it stays where it is until xsdk_save_wait says the call is done.
typedef struct {
	XAM_OVERLAPPED overlapped;
	XCONTENT_DATA data;
	uint32_t user_index;
	// XSDK_SAVE_CREATED or XSDK_SAVE_OPENED, once a mount is done.
	uint32_t disposition;
	uint32_t license_mask;
	// The flush's result, once xsdk_save_unmount has moved on to closing; ERROR_IO_PENDING before.
	uint32_t flush_result;
	bool flushing;
	char drive[XSDK_SAVE_DRIVE_BYTES];
} XsdkSave;

// The hard disk's device ID, from XAM's device enumerator, looked up on the first call without an overlapped;
// ERROR_DEVICE_NOT_CONNECTED if it lists none.
uint32_t xsdk_save_device(uint32_t *device_id);
// Mounts the user's package file_name, up to 41 characters, on the hard disk as drive, making it if it isn't there;
// display_name, UTF-16 and up to 127 characters, is what the dashboard lists.
uint32_t xsdk_save_mount(
	XsdkSave *save, uint32_t user_index, const char *drive, const char *file_name, const uint16_t *display_name);
// Flushes the package, then closes it; files still open in it fail from then on.
uint32_t xsdk_save_unmount(XsdkSave *save);
// As xsdk_xam_wait, for the call running on the save; an unmount is done once its close is.
uint32_t xsdk_save_wait(XsdkSave *save, uint32_t timeout_ms);

// Ends the title as exit does, atexit handlers, files, the GPU, the display and the log, then starts the .xex at path,
// with size_bytes of data, up to XSDK_LAUNCH_DATA_MAX_BYTES, as its launch data. Returns only if it can't, before
// anything ends: -1, errno ENOENT where there's no .xex, E2BIG for more data, or what the path or the link gave.
int xsdk_launch(const char *path, const void *data, size_t size_bytes);
// The launch data the title was started with, up to size_bytes of it into buffer; returns its size, 0 for none.
size_t xsdk_launch_data(void *buffer, size_t size_bytes);

#ifdef __cplusplus
}
#endif

#endif
