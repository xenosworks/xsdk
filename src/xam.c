#include "xsdk/xam.h"

#include "xsdk/kernel.h"
#include "xsdk/log.h"

enum {
	// NT counts a relative timeout in 100 ns units, negative.
	TIMEOUT_UNITS_PER_MS = 10000,
};

static void close_event(XAM_OVERLAPPED *overlapped)
{
	if (overlapped->event)
		(void)NtClose(overlapped->event);
	overlapped->event = 0;
}

// A notification event, so it stays set once XAM sets it as the call completes (xenia:src/xenia/kernel/
// kernel_state.cc:696-720).
void xsdk_xam_prepare(XAM_OVERLAPPED *overlapped)
{
	*overlapped = (XAM_OVERLAPPED){ 0 };
	const NTSTATUS status = NtCreateEvent(&overlapped->event, NULL, NOTIFICATION_EVENT, 0);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("xam: NtCreateEvent failed: 0x%08X", (unsigned)status);
		overlapped->event = 0;
	}
}

uint32_t xsdk_xam_returned(XAM_OVERLAPPED *overlapped, uint32_t returned)
{
	if (returned != ERROR_IO_PENDING)
		close_event(overlapped);
	return returned;
}

// XamGetOverlappedResult is asked only once the event is set, or with no event: Xenia's waits for the event whatever
// its third argument says (xenia:src/xenia/kernel/xam/xam_msg.cc:113-135), and the console's gives ERROR_IO_INCOMPLETE
// with 0 (results/2026-10-19-milestone-51/probe_xam.log).
uint32_t xsdk_xam_wait(XAM_OVERLAPPED *overlapped, uint32_t timeout_ms, uint32_t *length)
{
	if (overlapped->event) {
		const int64_t timeout = -(int64_t)timeout_ms * TIMEOUT_UNITS_PER_MS;
		if (NtWaitForSingleObjectEx(overlapped->event, USER_MODE, 0, &timeout) == STATUS_TIMEOUT)
			return ERROR_IO_INCOMPLETE;
	}
	// The console always had a length to write; whether it takes NULL is unknown.
	uint32_t unused_length;
	const uint32_t result = XamGetOverlappedResult(overlapped, length ? length : &unused_length, 0);
	if (result != ERROR_IO_INCOMPLETE)
		close_event(overlapped);
	return result;
}

// A sign-in state of 0 is nobody, as the console gave for all four with no profile (probe_xam.log).
uint32_t xsdk_xam_signed_in_user(uint32_t *user_index, char gamertag[XSDK_XAM_GAMERTAG_BYTES])
{
	*user_index = XSDK_XAM_USER_NONE;
	gamertag[0] = '\0';
	for (uint32_t user = 0; user < XSDK_XAM_USERS; user++) {
		if (XamUserGetSigninState(user) == 0)
			continue;
		const uint32_t result = XamUserGetName(user, gamertag, XSDK_XAM_GAMERTAG_BYTES);
		if (result != ERROR_SUCCESS) {
			gamertag[0] = '\0';
			return result;
		}
		gamertag[XSDK_XAM_GAMERTAG_BYTES - 1] = '\0';
		*user_index = user;
		return result;
	}
	return ERROR_NO_SUCH_USER;
}

// Flags 0, the default keyboard, as Xenia's shim and the probe call it (xenia:src/xenia/kernel/xam/xam_ui.cc:408-462).
uint32_t xsdk_xam_keyboard(uint32_t user_index, const uint16_t *title, const uint16_t *description,
	const uint16_t *default_text, uint16_t *text, uint32_t text_chars, XAM_OVERLAPPED *overlapped)
{
	(void)xsdk_system_ui_open();
	xsdk_xam_prepare(overlapped);
	return xsdk_xam_returned(
		overlapped, XamShowKeyboardUI(user_index, 0, default_text, title, description, text, text_chars, overlapped));
}

// The first button focused, flags 0 for no icon (xenia:src/xenia/kernel/xam/xam_ui.cc:268-323).
uint32_t xsdk_xam_message_box(uint32_t user_index, const uint16_t *title, const uint16_t *text,
	const uint16_t *const *buttons, uint32_t button_count, uint32_t *pressed, XAM_OVERLAPPED *overlapped)
{
	(void)xsdk_system_ui_open();
	xsdk_xam_prepare(overlapped);
	return xsdk_xam_returned(
		overlapped, XamShowMessageBoxUI(user_index, title, text, button_count, buttons, 0, 0, pressed, overlapped));
}
