#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "bar_scene.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/splash.h"
#include "xsdk/thread.h"

// Milestone 51 step 1 (docs/scope/51-saves-and-profiles.md): what XAM's content, profile and dialog calls do on the
// console, and how their overlapped calls complete. game:\xam_start.txt names the cases to run, in order; without it,
// those that need nobody. A thread presents the bar scene throughout, so XAM's dialogs and the Guide can show
// (docs/HARDWARE.md, "The Guide shows through VdSwap"), and every wait for XAM gives up after a timeout. After a case
// that needs a person, or with XAM's UI open, the title runs until Back, as ending with the Guide open hangs.

enum {
	SPLASH_MS = 2000,
	BUFFERS = 3,
	SWAP_INTERVAL = 1,
	BAR_ARGB = 0xFFFF8000,
	BEGIN_TIMEOUT_MS = 5000,
	PRESENT_HARDWARE_THREAD = 2,
	MAX_MISSES_LOGGED = 4,
	// XN_SYS_UI, its parameter nonzero while XAM's UI shows (xenia:src/xenia/kernel/xam/xam_ui.cc:72-98); every
	// area's notifications, at Xenia's highest version (xam_notify.cc:21-28).
	XN_SYS_UI = 9,
	NOTIFY_MAX_VERSION = 10,
	QUICK_TIMEOUT_MS = 10000,
	PERSON_TIMEOUT_MS = 120000,
	// XN_SYS_UI may come a little after a dialog's call returns (xam_ui.cc:95-98).
	UI_SETTLE_MS = 1000,
	POLL_MS = 20,
	PATTERN = 0xA5,
	HEX_LINE_BYTES = 32,
	WHAT_BYTES = 160,
	TEXT_BYTES = 128,
	CASE_NAME_BYTES = 32,
	MAX_RUN = 32,
	// XCONTENTTYPE's saved game (xenia:src/xenia/xbox.h:348-349); XamContentCreateEx's flags open existing and open
	// always (xenia:src/xenia/kernel/xam/xam_content.cc:149-189).
	CONTENT_SAVED_GAME = 1,
	CONTENT_OPEN_EXISTING = 3,
	CONTENT_OPEN_ALWAYS = 4,
	// Xenia's hard disk, its type and its ID (xenia:src/xenia/kernel/xam/xam_content_device.h:19-27).
	DEVICE_TYPE_HDD = 1,
	XENIA_HDD_DEVICE = 1,
	// A device ID the enumerators give no device.
	NO_DEVICE = 0x7F,
	MAX_DEVICES = 8,
	DEVICE_NAME_CHARS = 28,
	MAX_CONTENT_ITEMS = 16,
	MAX_PASSES = 4,
	USER_NAME_BYTES = 16,
	SIGNIN_INFO_FLAGS = 3,
	// Invert Y and vibration (xenia:src/xenia/kernel/xam/user_profile.cc:31-34).
	SETTING_INVERT_Y = 0x10040002,
	SETTING_VIBRATION = 0x10040003,
	SETTING_COUNT = 2,
	SETTINGS_BYTES = 256,
	KEYBOARD_CHARS = 32,
	BUTTONS = 2,
	GUIDE_ROUNDS = 2,
	// The hard disk's Content folder: a profile's folder, a title's, a type's, then the packages.
	MAX_DEPTH = 4,
	MAX_PATHS = 8192,
	PATH_BYTES = 112,
};

_Static_assert(sizeof(XAM_OVERLAPPED) == 0x1C && sizeof(XCONTENT_DATA) == 0x134 &&
		sizeof(CONTENT_DEVICE_DATA) == 0x50 && sizeof(USER_SIGNIN_INFO) == 40,
	"XAM's layouts");

static const uint32_t PATTERN_WORD = 0xA5A5A5A5;
static const uint64_t PATTERN_XUID = 0xA5A5A5A5A5A5A5A5ull;

typedef enum { UNATTENDED, ON_REQUEST, PERSON } CaseKind;

typedef struct {
	const char *name;
	CaseKind kind;
	const char *what;
	void (*run)(void);
} ProbeCase;

// A call made on a thread of its own, as it may not return until a person answers a dialog.
typedef struct {
	const char *what;
	uint32_t (*call)(void);
	volatile uint32_t result;
	volatile bool done;
	volatile uint64_t end_us;
} BlockingCall;

// The Content folder's entries, relative to it, sorted.
typedef struct {
	char (*paths)[PATH_BYTES];
	uint32_t count;
	bool truncated;
} Listing;

static const char *const start_path = "game:\\xam_start.txt";
static const char *const content_root = "\\Device\\Harddisk0\\Partition1\\Content";
static const char *const save_root = "save";
static const char *const save_file = "save:\\probe.dat";
static const char *const open_file_path = "save:\\open.dat";
static const char *const unmounted_root = "xsdkprb";

// The users each make a package of their own, so none finds another's.
static const uint32_t save_users[] = { 0xFE, 0xFF, 0 };
enum { SAVE_USERS = sizeof save_users / sizeof save_users[0], USER_0 = 2 };

static const uint16_t keyboard_default[] = u"xsdk";
static const uint16_t accept_title[] = u"xsdk probe: keyboard 1 of 2";
static const uint16_t accept_description[] = u"Type a word and accept it.";
static const uint16_t cancel_title[] = u"xsdk probe: keyboard 2 of 2";
static const uint16_t cancel_description[] = u"Press B to cancel.";
static const uint16_t null_title[] = u"xsdk probe: keyboard, no overlapped";
static const uint16_t null_description[] = u"Type a word and accept it.";
static const uint16_t guide_title[] = u"xsdk probe: keyboard asked for under the Guide";
static const uint16_t guide_description[] = u"Press B to cancel.";
static const uint16_t box_title[] = u"xsdk probe";
static const uint16_t box_text[] = u"Press the second button.";
static const uint16_t null_box_text[] = u"No overlapped. Press the second button.";
static const uint16_t guide_box_text[] = u"Asked for under the Guide. Press any button.";
static const uint16_t first_button[] = u"First";
static const uint16_t second_button[] = u"Second";
static const uint16_t *const buttons[BUTTONS] = { first_button, second_button };

static uint64_t start_us;
static volatile bool stop_presenting;
static volatile uint32_t frames_presented;
static HANDLE listener;
static bool ui_open;
static uint32_t ui_closings;
static uint32_t device_ids[MAX_DEVICES];
static uint32_t device_types[MAX_DEVICES];
static uint32_t device_count;
static bool devices_listed;
// Static, as is all XAM may write: a call that times out may still write it.
static XCONTENT_DATA packages[SAVE_USERS];
static XAM_OVERLAPPED close_overlapped;
static XAM_OVERLAPPED *waited;
static uint32_t waited_length;
static uint16_t keyboard_text[KEYBOARD_CHARS];
static uint32_t pressed;

static uint64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

static unsigned ms_since(uint64_t start)
{
	return (unsigned)((now_us() - start) / 1000);
}

static void *present_frames(void *argument)
{
	const BarScene *const scene = argument;
	(void)xsdk_thread_move(PRESENT_HARDWARE_THREAD);
	uint32_t misses = 0;
	for (uint32_t frame = 0; !stop_presenting;) {
		uint32_t address;
		if (!NT_SUCCESS(xsdk_present_begin(&address, BEGIN_TIMEOUT_MS))) {
			if (misses++ < MAX_MISSES_LOGGED)
				xsdk_log_write("present: no front buffer came free in %u ms at frame %u", (unsigned)BEGIN_TIMEOUT_MS,
					(unsigned)frame);
			continue;
		}
		bar_scene_draw(scene, frame, address);
		xsdk_present_end();
		frames_presented = ++frame;
	}
	return NULL;
}

static void take_notifications(void)
{
	uint32_t id = 0;
	uint32_t param = 0;
	while (listener && XNotifyGetNext(listener, 0, &id, &param)) {
		xsdk_log_write("notify: 0x%08X, parameter 0x%08X, at %u ms, frame %u", (unsigned)id, (unsigned)param,
			ms_since(start_us), (unsigned)frames_presented);
		if (id == XN_SYS_UI) {
			ui_open = param != 0;
			ui_closings += param == 0;
		}
	}
}

static void pause_ms(uint32_t milliseconds)
{
	const uint64_t start = now_us();
	do {
		take_notifications();
		(void)xsdk_thread_sleep(POLL_MS);
	} while (ms_since(start) < milliseconds);
}

static void log_bytes(const char *what, const void *bytes, uint32_t length_bytes)
{
	const uint8_t *const b = bytes;
	for (uint32_t at = 0; at < length_bytes; at += HEX_LINE_BYTES) {
		char line[HEX_LINE_BYTES * 3 + 1] = "";
		size_t used = 0;
		for (uint32_t i = at; i < length_bytes && i < at + HEX_LINE_BYTES; i++)
			used += (size_t)snprintf(line + used, sizeof line - used, " %02X", b[i]);
		xsdk_log_write("%s +0x%03X:%s", what, (unsigned)at, line);
	}
}

// UTF-16 as ASCII, '?' for anything else.
static void text_of(const uint16_t *chars, uint32_t count, char *text, size_t text_bytes)
{
	size_t used = 0;
	for (uint32_t i = 0; i < count && chars[i] && used + 1 < text_bytes; i++)
		text[used++] = chars[i] >= 0x20 && chars[i] < 0x7F ? (char)chars[i] : '?';
	text[used] = '\0';
}

static const char *event_state(HANDLE event)
{
	if (!event)
		return "none";
	const int64_t now = 0;
	return NtWaitForSingleObjectEx(event, USER_MODE, 0, &now) == STATUS_SUCCESS ? "set" : "not set";
}

// Zeroed with an event of its own; with none, the fields XAM writes hold the pattern, to show when it writes them.
static void overlapped_start(XAM_OVERLAPPED *o, bool with_event)
{
	const uint32_t fill = with_event ? 0 : PATTERN_WORD;
	*o = (XAM_OVERLAPPED){ .result = fill, .length = fill, .extended_error = fill };
	if (!with_event)
		return;
	const NTSTATUS status = NtCreateEvent(&o->event, NULL, NOTIFICATION_EVENT, 0);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("NtCreateEvent failed: 0x%08X", (unsigned)status);
		o->event = 0;
	}
}

static void close_event(XAM_OVERLAPPED *o)
{
	if (o->event)
		(void)NtClose(o->event);
	o->event = 0;
}

static void log_overlapped(const char *what, const char *when, const XAM_OVERLAPPED *o, const char *event)
{
	xsdk_log_write(
		"%s: overlapped %s: result 0x%08X, length 0x%08X, context 0x%08X, event 0x%08X (%s), routine "
		"0x%08X, routine context 0x%08X, extended error 0x%08X",
		what, when, (unsigned)o->result, (unsigned)o->length, (unsigned)o->context, (unsigned)o->event, event,
		(unsigned)o->completion_routine, (unsigned)o->completion_context, (unsigned)o->extended_error);
}

// Waits up to timeout_ms for the call to complete: its event set, or with none its result neither the pattern nor
// pending.
static bool overlapped_wait(const char *what, const XAM_OVERLAPPED *o, uint32_t timeout_ms)
{
	const uint64_t start = now_us();
	const uint32_t frames = frames_presented;
	const int64_t poll = -(int64_t)POLL_MS * 10000;
	for (;;) {
		take_notifications();
		NTSTATUS status = STATUS_TIMEOUT;
		if (o->event)
			status = NtWaitForSingleObjectEx(o->event, USER_MODE, 0, &poll);
		else if (o->result != PATTERN_WORD && o->result != ERROR_IO_PENDING)
			status = STATUS_SUCCESS;
		if (status != STATUS_TIMEOUT) {
			xsdk_log_write("%s: completed after %u ms (wait 0x%08X), %u frames presented meanwhile", what,
				ms_since(start), (unsigned)status, (unsigned)(frames_presented - frames));
			return true;
		}
		if (ms_since(start) >= timeout_ms) {
			xsdk_log_write("%s: FAILED, not complete in %u ms", what, (unsigned)timeout_ms);
			return false;
		}
		if (!o->event)
			(void)xsdk_thread_sleep(POLL_MS);
	}
}

static void log_results(const char *what, XAM_OVERLAPPED *o)
{
	for (uint32_t wait = 0; wait < 2; wait++) {
		uint32_t length = PATTERN_WORD;
		const uint32_t result = XamGetOverlappedResult(o, &length, wait);
		xsdk_log_write("%s: XamGetOverlappedResult(wait %u) once complete = 0x%08X, length 0x%08X", what,
			(unsigned)wait, (unsigned)result, (unsigned)length);
	}
}

// Logs a call's return and its overlapped as it returned, and with peek what XamGetOverlappedResult gave then, not
// waiting; call it straight after the call.
static void log_return(const char *what, uint32_t returned, XAM_OVERLAPPED *o, bool peek)
{
	const XAM_OVERLAPPED on_return = *o;
	const char *const event_on_return = event_state(o->event);
	uint32_t peeked = 0;
	uint32_t peeked_length = PATTERN_WORD;
	const uint64_t peek_start = now_us();
	if (peek)
		peeked = XamGetOverlappedResult(o, &peeked_length, 0);
	const unsigned peek_us = (unsigned)(now_us() - peek_start);
	xsdk_log_write("%s returned 0x%08X", what, (unsigned)returned);
	log_overlapped(what, "on return", &on_return, event_on_return);
	if (peek)
		xsdk_log_write("%s: XamGetOverlappedResult(wait 0) straight after = 0x%08X in %u us, length 0x%08X", what,
			(unsigned)peeked, peek_us, (unsigned)peeked_length);
}

// Waits up to timeout_ms if the call is pending, and with results logs what XamGetOverlappedResult gives once it's
// complete. Gives the overlapped's result once complete, the return if it didn't pend, or ERROR_IO_PENDING if it never
// completed, its event left open as XAM may still set it.
static uint32_t complete(const char *what, uint32_t returned, XAM_OVERLAPPED *o, uint32_t timeout_ms, bool results)
{
	if (returned != ERROR_IO_PENDING) {
		close_event(o);
		return returned;
	}
	if (!overlapped_wait(what, o, timeout_ms))
		return ERROR_IO_PENDING;
	log_overlapped(what, "complete", o, event_state(o->event));
	if (results)
		log_results(what, o);
	close_event(o);
	return o->result;
}

static uint32_t finish(const char *what, uint32_t returned, XAM_OVERLAPPED *o, uint32_t timeout_ms, bool peek)
{
	log_return(what, returned, o, peek);
	return complete(what, returned, o, timeout_ms, peek);
}

static void *call_blocking(void *argument)
{
	BlockingCall *const c = argument;
	c->result = c->call();
	c->end_us = now_us();
	c->done = true;
	return NULL;
}

// Whether the call returned within timeout_ms; one that doesn't is left running.
static bool run_blocking(BlockingCall *c, uint32_t timeout_ms)
{
	c->done = false;
	pthread_attr_t attributes;
	(void)pthread_attr_init(&attributes);
	(void)pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
	pthread_t thread;
	const uint64_t start = now_us();
	const uint32_t frames = frames_presented;
	const int created = pthread_create(&thread, &attributes, call_blocking, c);
	(void)pthread_attr_destroy(&attributes);
	if (created != 0) {
		xsdk_log_write("%s: FAILED, pthread_create %d", c->what, created);
		return false;
	}
	while (!c->done && ms_since(start) < timeout_ms) {
		take_notifications();
		(void)xsdk_thread_sleep(POLL_MS);
	}
	if (!c->done) {
		xsdk_log_write("%s: FAILED, no return in %u ms; left running", c->what, (unsigned)timeout_ms);
		return false;
	}
	xsdk_log_write("%s returned 0x%08X after %u ms, %u frames presented meanwhile", c->what, (unsigned)c->result,
		(unsigned)((c->end_us - start) / 1000), (unsigned)(frames_presented - frames));
	return true;
}

static uint32_t wait_for_result(void)
{
	return XamGetOverlappedResult(waited, &waited_length, 1);
}

// Waits for XAM's UI to close, closings having been ui_closings before it opened.
static bool wait_ui_closed(const char *what, uint32_t closings, uint32_t timeout_ms)
{
	const uint64_t start = now_us();
	while (ui_closings == closings || ui_open) {
		if (ms_since(start) >= timeout_ms) {
			xsdk_log_write("%s: XAM's UI didn't close in %u ms", what, (unsigned)timeout_ms);
			return false;
		}
		take_notifications();
		(void)xsdk_thread_sleep(POLL_MS);
	}
	xsdk_log_write("%s: XAM's UI closed after %u ms", what, ms_since(start));
	return true;
}

static void log_signin_states(const char *what)
{
	uint32_t states[4];
	for (uint32_t user = 0; user < 4; user++)
		states[user] = XamUserGetSigninState(user);
	xsdk_log_write("%s: XamUserGetSigninState of users 0 to 3: %u %u %u %u", what, (unsigned)states[0],
		(unsigned)states[1], (unsigned)states[2], (unsigned)states[3]);
}

static void run_users(void)
{
	log_signin_states("users");
	static const uint32_t masks[] = { 1, 2, 4, 7 };
	for (uint32_t i = 0; i < sizeof masks / sizeof masks[0]; i++) {
		uint64_t xuid = PATTERN_XUID;
		const uint32_t result = XamUserGetXUID(0, masks[i], &xuid);
		xsdk_log_write("users: XamUserGetXUID(user 0, type mask %u) = 0x%08X, XUID %016llX", (unsigned)masks[i],
			(unsigned)result, (unsigned long long)xuid);
	}
	char name[USER_NAME_BYTES];
	memset(name, PATTERN, sizeof name);
	const uint32_t result = XamUserGetName(0, name, sizeof name);
	xsdk_log_write("users: XamUserGetName(user 0, %u bytes) = 0x%08X, '%.*s'", (unsigned)sizeof name, (unsigned)result,
		result == ERROR_SUCCESS ? (int)strnlen(name, sizeof name) : 0, name);
	log_bytes("users: the name", name, sizeof name);
	for (uint32_t flags = 0; flags < SIGNIN_INFO_FLAGS; flags++) {
		USER_SIGNIN_INFO info;
		memset(&info, PATTERN, sizeof info);
		char what[WHAT_BYTES];
		snprintf(what, sizeof what, "users: XamUserGetSigninInfo(user 0, flags %u)", (unsigned)flags);
		xsdk_log_write("%s = 0x%08X", what, (unsigned)XamUserGetSigninInfo(0, flags, &info));
		log_bytes(what, &info, sizeof info);
	}
}

static void log_device(const char *what, const CONTENT_DEVICE_DATA *d)
{
	char name[DEVICE_NAME_CHARS + 1];
	text_of(d->name, DEVICE_NAME_CHARS, name, sizeof name);
	xsdk_log_write("%s: device 0x%08X, type 0x%08X, %llu bytes, %llu free, name '%s'", what, (unsigned)d->device_id,
		(unsigned)d->device_type, (unsigned long long)d->total_bytes, (unsigned long long)d->free_bytes, name);
}

static void note_device(const void *item)
{
	const CONTENT_DEVICE_DATA *const d = item;
	log_device("devices: enumerated", d);
	if (device_count < MAX_DEVICES) {
		device_ids[device_count] = d->device_id;
		device_types[device_count] = d->device_type;
		device_count++;
	}
}

static void log_content_item(const void *item)
{
	const XCONTENT_DATA *const c = item;
	char name[TEXT_BYTES];
	text_of(c->display_name, sizeof c->display_name / 2, name, sizeof name);
	xsdk_log_write(
		"content: enumerated device 0x%08X, type 0x%08X, display name '%s', file name '%.42s', padding "
		"%02X %02X",
		(unsigned)c->device_id, (unsigned)c->content_type, name, c->file_name, c->padding[0], c->padding[1]);
}

// Takes an enumerator's items, logging each, until it says there are no more, then closes it.
static void take_items(const char *what, HANDLE enumerator, void *buffer, uint32_t item_bytes, uint32_t max_items,
	void (*log_item)(const void *item))
{
	for (uint32_t pass = 0; pass < MAX_PASSES; pass++) {
		uint32_t items = PATTERN_WORD;
		const uint32_t result = XamEnumerate(enumerator, 0, buffer, item_bytes * max_items, &items, NULL);
		xsdk_log_write(
			"%s: XamEnumerate pass %u = 0x%08X, 0x%08X items", what, (unsigned)pass, (unsigned)result, (unsigned)items);
		if (result != ERROR_SUCCESS)
			break;
		for (uint32_t i = 0; i < items && i < max_items; i++)
			log_item((const uint8_t *)buffer + i * item_bytes);
	}
	xsdk_log_write("%s: NtClose(enumerator) = 0x%08X", what, (unsigned)NtClose(enumerator));
}

static void list_devices(void)
{
	static CONTENT_DEVICE_DATA items[MAX_DEVICES];
	devices_listed = true;
	device_count = 0;
	uint32_t bytes = PATTERN_WORD;
	HANDLE enumerator = 0;
	const uint32_t result = XamContentCreateDeviceEnumerator(CONTENT_SAVED_GAME, 0, MAX_DEVICES, &bytes, &enumerator);
	xsdk_log_write(
		"devices: XamContentCreateDeviceEnumerator(saved games, %u items) = 0x%08X, 0x%08X bytes, handle "
		"0x%08X",
		(unsigned)MAX_DEVICES, (unsigned)result, (unsigned)bytes, (unsigned)enumerator);
	if (result == ERROR_SUCCESS)
		take_items("devices", enumerator, items, sizeof items[0], MAX_DEVICES, note_device);
}

// The first enumerated device of the hard disk's type, as Xenia has it, else Xenia's hard disk.
static uint32_t hdd_device(void)
{
	if (!devices_listed)
		list_devices();
	for (uint32_t i = 0; i < device_count; i++) {
		if (device_types[i] == DEVICE_TYPE_HDD)
			return device_ids[i];
	}
	static bool told;
	if (!told)
		xsdk_log_write("devices: none of type %u enumerated; using device %u, Xenia's hard disk",
			(unsigned)DEVICE_TYPE_HDD, (unsigned)XENIA_HDD_DEVICE);
	told = true;
	return XENIA_HDD_DEVICE;
}

static void run_devices(void)
{
	list_devices();
	for (uint32_t i = 0; i < device_count; i++) {
		const uint32_t id = device_ids[i];
		CONTENT_DEVICE_DATA data;
		memset(&data, PATTERN, sizeof data);
		uint32_t result = XamContentGetDeviceData(id, &data);
		xsdk_log_write("devices: XamContentGetDeviceData(0x%08X) = 0x%08X", (unsigned)id, (unsigned)result);
		if (result == ERROR_SUCCESS)
			log_device("devices: its data", &data);
		uint16_t name[DEVICE_NAME_CHARS];
		memset(name, PATTERN, sizeof name);
		result = XamContentGetDeviceName(id, name, DEVICE_NAME_CHARS);
		char text[DEVICE_NAME_CHARS + 1];
		text_of(name, DEVICE_NAME_CHARS, text, sizeof text);
		xsdk_log_write("devices: XamContentGetDeviceName(0x%08X, %u chars) = 0x%08X, '%s'", (unsigned)id,
			(unsigned)DEVICE_NAME_CHARS, (unsigned)result, result == ERROR_SUCCESS ? text : "");
		xsdk_log_write("devices: XamContentGetDeviceState(0x%08X, no overlapped) = 0x%08X", (unsigned)id,
			(unsigned)XamContentGetDeviceState(id, NULL));
	}
	xsdk_log_write("devices: content goes on device 0x%08X", (unsigned)hdd_device());
}

static void run_overlapped(void)
{
	const uint32_t device = hdd_device();
	static XAM_OVERLAPPED o;
	// With an event and with none, each looked at straight after the call.
	for (uint32_t variant = 0; variant < 2; variant++) {
		const bool with_event = variant == 0;
		char what[WHAT_BYTES];
		snprintf(what, sizeof what, "overlapped: XamContentGetDeviceState(0x%08X), %s", (unsigned)device,
			with_event ? "an event" : "no event");
		overlapped_start(&o, with_event);
		log_overlapped(what, "before the call", &o, event_state(o.event));
		const uint64_t start = now_us();
		const uint32_t returned = XamContentGetDeviceState(device, &o);
		const unsigned call_us = (unsigned)(now_us() - start);
		(void)finish(what, returned, &o, QUICK_TIMEOUT_MS, true);
		xsdk_log_write("%s: the call took %u us", what, call_us);
	}
	// Told to wait straight after the call, on a thread of its own in case it never returns.
	static BlockingCall wait_call = { .what =
										  "overlapped: XamGetOverlappedResult(wait 1) straight after "
										  "XamContentGetDeviceState",
		.call = wait_for_result };
	overlapped_start(&o, true);
	waited = &o;
	waited_length = PATTERN_WORD;
	const uint32_t returned = XamContentGetDeviceState(device, &o);
	log_return("overlapped: XamContentGetDeviceState, then waited", returned, &o, false);
	if (run_blocking(&wait_call, QUICK_TIMEOUT_MS))
		xsdk_log_write("%s: length 0x%08X", wait_call.what, (unsigned)waited_length);
	(void)complete("overlapped: XamContentGetDeviceState, then waited", returned, &o, QUICK_TIMEOUT_MS, false);
	overlapped_start(&o, true);
	(void)finish("overlapped: XamContentClose(xsdkprb), a root not mounted", XamContentClose(unmounted_root, &o), &o,
		QUICK_TIMEOUT_MS, true);
	overlapped_start(&o, true);
	(void)finish("overlapped: XamContentGetDeviceState(0x7F), no such device", XamContentGetDeviceState(NO_DEVICE, &o),
		&o, QUICK_TIMEOUT_MS, true);
	xsdk_log_write("overlapped: XamContentGetDeviceState(0x%08X, no overlapped) = 0x%08X", (unsigned)device,
		(unsigned)XamContentGetDeviceState(device, NULL));
}

static int compare_paths(const void *a, const void *b)
{
	return strcmp(a, b);
}

static void list_folder(Listing *listing, const char *relative, uint32_t depth)
{
	char path[PATH_BYTES * 2];
	snprintf(path, sizeof path, "%s%s", content_root, relative);
	DIR *const dir = opendir(path);
	if (!dir) {
		xsdk_log_write("listing: opendir(%s) failed, errno %d", path, errno);
		return;
	}
	for (struct dirent *e; (e = readdir(dir)) != NULL;) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
			continue;
		if (listing->count == MAX_PATHS) {
			listing->truncated = true;
			break;
		}
		char *const entry = listing->paths[listing->count];
		if ((size_t)snprintf(entry, PATH_BYTES, "%s\\%s", relative, e->d_name) >= PATH_BYTES) {
			listing->truncated = true;
			continue;
		}
		listing->count++;
		if (e->d_type == DT_DIR && depth < MAX_DEPTH)
			list_folder(listing, entry, depth + 1);
	}
	(void)closedir(dir);
}

static bool take_listing(Listing *listing, const char *when)
{
	*listing = (Listing){ .paths = malloc((size_t)MAX_PATHS * PATH_BYTES) };
	if (!listing->paths) {
		xsdk_log_write("listing: FAILED, no memory for %u paths", (unsigned)MAX_PATHS);
		return false;
	}
	const uint64_t start = now_us();
	list_folder(listing, "", 0);
	qsort(listing->paths, listing->count, PATH_BYTES, compare_paths);
	xsdk_log_write("listing %s: %u entries under %s%s, in %u ms", when, (unsigned)listing->count, content_root,
		listing->truncated ? ", CUT SHORT" : "", ms_since(start));
	return true;
}

static void log_new(const char *relative)
{
	char path[PATH_BYTES * 2];
	snprintf(path, sizeof path, "%s%s", content_root, relative);
	struct stat info;
	if (stat(path, &info) != 0)
		xsdk_log_write("listing: new %s, stat errno %d", path, errno);
	else
		xsdk_log_write("listing: new %s, a %s of %lld bytes", path, S_ISDIR(info.st_mode) ? "folder" : "file",
			(long long)info.st_size);
}

// Below 0 if before's path i comes first, above if after's j does, 0 if they're the same.
static int listing_order(const Listing *before, uint32_t i, const Listing *after, uint32_t j)
{
	if (i == before->count)
		return 1;
	if (j == after->count)
		return -1;
	return strcmp(before->paths[i], after->paths[j]);
}

// Lists the folder again and logs what's new and what's gone since before, which it frees.
static void log_changes(Listing *before)
{
	Listing after;
	if (!take_listing(&after, "after")) {
		free(before->paths);
		return;
	}
	uint32_t i = 0;
	uint32_t j = 0;
	uint32_t changes = 0;
	while (i < before->count || j < after.count) {
		const int order = listing_order(before, i, &after, j);
		changes += order != 0;
		if (order < 0)
			xsdk_log_write("listing: gone %s%s", content_root, before->paths[i++]);
		else if (order > 0)
			log_new(after.paths[j++]);
		else {
			i++;
			j++;
		}
	}
	xsdk_log_write("listing: %u changes", (unsigned)changes);
	free(after.paths);
	free(before->paths);
}

static const XCONTENT_DATA *package(uint32_t index)
{
	XCONTENT_DATA *const p = &packages[index];
	*p = (XCONTENT_DATA){ .device_id = hdd_device(), .content_type = CONTENT_SAVED_GAME };
	snprintf(p->file_name, sizeof p->file_name, "xsdkprobe_%02x", (unsigned)save_users[index]);
	char name[TEXT_BYTES];
	snprintf(name, sizeof name, "xsdk probe, user %02X", (unsigned)save_users[index]);
	for (size_t i = 0; name[i]; i++)
		p->display_name[i] = (uint16_t)name[i];
	return p;
}

static uint32_t create_content(uint32_t user, const XCONTENT_DATA *data, uint32_t flags)
{
	static XAM_OVERLAPPED o;
	static uint32_t disposition;
	static uint32_t license;
	disposition = PATTERN_WORD;
	license = PATTERN_WORD;
	char what[WHAT_BYTES];
	snprintf(what, sizeof what, "content: XamContentCreateEx(user 0x%02X, %s, device 0x%08X, %.42s, flags %u)",
		(unsigned)user, save_root, (unsigned)data->device_id, data->file_name, (unsigned)flags);
	overlapped_start(&o, true);
	const uint32_t result =
		finish(what, XamContentCreateEx(user, save_root, data, flags, &disposition, &license, 0, 0, &o), &o,
			QUICK_TIMEOUT_MS, false);
	xsdk_log_write("%s: result 0x%08X, disposition 0x%08X, license mask 0x%08X", what, (unsigned)result,
		(unsigned)disposition, (unsigned)license);
	return result;
}

static uint32_t close_content(void)
{
	overlapped_start(&close_overlapped, true);
	return finish("content: XamContentClose(save)", XamContentClose(save_root, &close_overlapped), &close_overlapped,
		QUICK_TIMEOUT_MS, false);
}

static void flush_content(void)
{
	static XAM_OVERLAPPED o;
	overlapped_start(&o, true);
	(void)finish("content: XamContentFlush(save)", XamContentFlush(save_root, &o), &o, QUICK_TIMEOUT_MS, false);
}

static void write_save(const char *text)
{
	FILE *const file = fopen(save_file, "wb");
	if (!file) {
		xsdk_log_write("content: fopen(%s, wb) failed, errno %d", save_file, errno);
		return;
	}
	const size_t written = fwrite(text, 1, strlen(text), file);
	const int closed = fclose(file);
	const bool wrote = written == strlen(text) && closed == 0;
	xsdk_log_write("content: wrote %u of %u bytes to %s, fclose %d, errno %d", (unsigned)written,
		(unsigned)strlen(text), save_file, closed, wrote ? 0 : errno);
}

static void read_save(const char *expected)
{
	FILE *const file = fopen(save_file, "rb");
	if (!file) {
		xsdk_log_write("content: fopen(%s, rb) failed, errno %d", save_file, errno);
		return;
	}
	char text[TEXT_BYTES];
	const size_t read = fread(text, 1, sizeof text - 1, file);
	(void)fclose(file);
	text[read] = '\0';
	xsdk_log_write("content: read %u bytes back from %s, %s", (unsigned)read, save_file,
		strcmp(text, expected) == 0 ? "as written" : "NOT as written");
}

// Makes the user's package, writes it, closes it, opens it again and reads it back.
static void save_round_trip(uint32_t index)
{
	const uint32_t user = save_users[index];
	const XCONTENT_DATA *const data = package(index);
	if (create_content(user, data, CONTENT_OPEN_ALWAYS) != ERROR_SUCCESS)
		return;
	char text[TEXT_BYTES];
	snprintf(text, sizeof text, "xsdk probe_xam, user 0x%02X\n", (unsigned)user);
	write_save(text);
	flush_content();
	(void)close_content();
	if (create_content(user, data, CONTENT_OPEN_EXISTING) != ERROR_SUCCESS)
		return;
	read_save(text);
	(void)close_content();
}

static void list_content(uint32_t user)
{
	static XCONTENT_DATA items[MAX_CONTENT_ITEMS];
	const uint32_t device = hdd_device();
	uint32_t bytes = PATTERN_WORD;
	HANDLE enumerator = 0;
	const uint32_t result =
		XamContentCreateEnumerator(user, device, CONTENT_SAVED_GAME, 0, MAX_CONTENT_ITEMS, &bytes, &enumerator);
	xsdk_log_write(
		"content: XamContentCreateEnumerator(user 0x%02X, device 0x%08X, saved games, %u items) = 0x%08X, "
		"0x%08X bytes, handle 0x%08X",
		(unsigned)user, (unsigned)device, (unsigned)MAX_CONTENT_ITEMS, (unsigned)result, (unsigned)bytes,
		(unsigned)enumerator);
	if (result == ERROR_SUCCESS)
		take_items("content", enumerator, items, sizeof items[0], MAX_CONTENT_ITEMS, log_content_item);
}

static void save_round_trips(uint32_t first, uint32_t count)
{
	Listing before;
	const bool listed = take_listing(&before, "before");
	for (uint32_t i = first; i < first + count; i++)
		save_round_trip(i);
	for (uint32_t i = first; i < first + count; i++)
		list_content(save_users[i]);
	if (listed)
		log_changes(&before);
}

static void run_content(void)
{
	save_round_trips(0, SAVE_USERS);
}

static void run_signed_in_content(void)
{
	log_signin_states("signed_in_content");
	save_round_trips(USER_0, 1);
}

static void run_delete(void)
{
	Listing before;
	const bool listed = take_listing(&before, "before");
	static XAM_OVERLAPPED o;
	for (uint32_t i = 0; i < SAVE_USERS; i++) {
		const XCONTENT_DATA *const data = package(i);
		char what[WHAT_BYTES];
		snprintf(what, sizeof what, "delete: XamContentDelete(user 0x%02X, %.42s)", (unsigned)save_users[i],
			data->file_name);
		overlapped_start(&o, true);
		(void)finish(what, XamContentDelete(save_users[i], data, &o), &o, QUICK_TIMEOUT_MS, false);
	}
	for (uint32_t i = 0; i < SAVE_USERS; i++)
		list_content(save_users[i]);
	if (listed)
		log_changes(&before);
}

static NTSTATUS open_file(const char *path, uint32_t disposition, HANDLE *file)
{
	const ANSI_STRING name = {
		.length = (uint16_t)strlen(path), .maximum_length = (uint16_t)(strlen(path) + 1), .pointer = path
	};
	const OBJECT_ATTRIBUTES attributes = { .root_directory = OB_DOS_DEVICES, .name = &name };
	IO_STATUS_BLOCK io_status;
	return NtCreateFile(file, GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE, &attributes, &io_status, NULL,
		FILE_ATTRIBUTE_NORMAL, 0, disposition, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
}

static NTSTATUS write_text(HANDLE file, const char *text)
{
	IO_STATUS_BLOCK io_status;
	return NtWriteFile(file, 0, NULL, NULL, &io_status, text, (uint32_t)strlen(text), NULL);
}

// What a file open in a package does once the package closes, and what of it the package keeps.
static void run_close_open_file(void)
{
	uint32_t index = 0;
	while (
		index < SAVE_USERS && create_content(save_users[index], package(index), CONTENT_OPEN_ALWAYS) != ERROR_SUCCESS)
		index++;
	if (index == SAVE_USERS) {
		xsdk_log_write("close_open_file: no user could make a package");
		return;
	}
	HANDLE file;
	NTSTATUS status = open_file(open_file_path, FILE_OVERWRITE_IF, &file);
	xsdk_log_write("close_open_file: NtCreateFile(%s) = 0x%08X", open_file_path, (unsigned)status);
	if (!NT_SUCCESS(status)) {
		(void)close_content();
		return;
	}
	xsdk_log_write("close_open_file: a write before the close = 0x%08X", (unsigned)write_text(file, "before;"));
	const uint32_t closed = close_content();
	xsdk_log_write("close_open_file: a write after the close = 0x%08X", (unsigned)write_text(file, "after;"));
	char text[TEXT_BYTES] = "";
	IO_STATUS_BLOCK io_status = { .information = PATTERN_WORD };
	const uint64_t offset = 0;
	status = NtReadFile(file, 0, NULL, NULL, &io_status, text, sizeof text - 1, &offset);
	xsdk_log_write("close_open_file: a read after the close = 0x%08X, %u bytes, '%.*s'", (unsigned)status,
		(unsigned)io_status.information, NT_SUCCESS(status) ? (int)strnlen(text, sizeof text) : 0, text);
	xsdk_log_write("close_open_file: NtClose(the file) = 0x%08X", (unsigned)NtClose(file));
	if (closed == ERROR_IO_PENDING &&
		overlapped_wait(
			"close_open_file: XamContentClose, once the file closed", &close_overlapped, QUICK_TIMEOUT_MS)) {
		log_overlapped("close_open_file", "complete", &close_overlapped, event_state(close_overlapped.event));
		close_event(&close_overlapped);
	}
	if (create_content(save_users[index], package(index), CONTENT_OPEN_EXISTING) != ERROR_SUCCESS)
		return;
	struct stat info;
	if (stat(open_file_path, &info) != 0)
		xsdk_log_write("close_open_file: stat(%s) after opening the package again, errno %d", open_file_path, errno);
	else
		xsdk_log_write("close_open_file: %s holds %lld bytes after opening the package again", open_file_path,
			(long long)info.st_size);
	(void)close_content();
}

static void run_settings(void)
{
	static const uint32_t ids[SETTING_COUNT] = { SETTING_INVERT_Y, SETTING_VIBRATION };
	static uint8_t buffer[SETTINGS_BYTES];
	static uint32_t bytes;
	static XAM_OVERLAPPED o;
	bytes = 0;
	uint32_t result = XamUserReadProfileSettings(0, 0, 0, NULL, SETTING_COUNT, ids, &bytes, NULL, NULL);
	xsdk_log_write("settings: XamUserReadProfileSettings(user 0, invert Y and vibration, no buffer) = 0x%08X, %u bytes",
		(unsigned)result, (unsigned)bytes);
	if (bytes == 0 || bytes > sizeof buffer)
		bytes = sizeof buffer;
	const uint32_t given = bytes;
	memset(buffer, PATTERN, sizeof buffer);
	overlapped_start(&o, true);
	result = finish("settings: XamUserReadProfileSettings(user 0, invert Y and vibration)",
		XamUserReadProfileSettings(0, 0, 0, NULL, SETTING_COUNT, ids, &bytes, buffer, &o), &o, QUICK_TIMEOUT_MS, false);
	xsdk_log_write(
		"settings: result 0x%08X, %u bytes given, 0x%08X after", (unsigned)result, (unsigned)given, (unsigned)bytes);
	log_bytes("settings: the buffer", buffer, given);
}

static void run_signin_ui(void)
{
	log_signin_states("signin_ui");
	const uint32_t closings = ui_closings;
	const uint32_t result = XamShowSigninUI(1, 0);
	xsdk_log_write("signin_ui: XamShowSigninUI(1 pane, flags 0) = 0x%08X, XamIsUIActive %u", (unsigned)result,
		(unsigned)XamIsUIActive());
	if (result == ERROR_SUCCESS || result == ERROR_IO_PENDING)
		(void)wait_ui_closed("signin_ui", closings, PERSON_TIMEOUT_MS);
	run_users();
}

static uint32_t show_keyboard(const uint16_t *title, const uint16_t *description, XAM_OVERLAPPED *o)
{
	memset(keyboard_text, PATTERN, sizeof keyboard_text);
	return XamShowKeyboardUI(0, 0, keyboard_default, title, description, keyboard_text, KEYBOARD_CHARS, o);
}

static void settle_ui(const char *what, uint32_t closings);

static void log_keyboard_text(const char *what)
{
	char text[KEYBOARD_CHARS + 1];
	text_of(keyboard_text, KEYBOARD_CHARS, text, sizeof text);
	xsdk_log_write("%s: the text '%s'", what, text);
	log_bytes(what, keyboard_text, sizeof keyboard_text);
}

static void run_keyboard(void)
{
	static XAM_OVERLAPPED o;
	const uint32_t closings = ui_closings;
	// Accepted, looked at straight after the call, not waiting.
	overlapped_start(&o, true);
	const uint32_t frames = frames_presented;
	(void)finish(
		"keyboard: accepted", show_keyboard(accept_title, accept_description, &o), &o, PERSON_TIMEOUT_MS, true);
	xsdk_log_write("keyboard: accepted, XamIsUIActive %u, %u frames", (unsigned)XamIsUIActive(),
		(unsigned)(frames_presented - frames));
	log_keyboard_text("keyboard: accepted");
	// XAM refuses a dialog with 5 until its UI says it closed, a moment after the call completes.
	settle_ui("keyboard: accepted", closings);
	// Cancelled, with XamGetOverlappedResult told to wait straight after the call.
	static BlockingCall wait_call = { .what =
										  "keyboard: XamGetOverlappedResult(wait 1) straight after the keyboard "
										  "opened",
		.call = wait_for_result };
	overlapped_start(&o, true);
	waited = &o;
	waited_length = PATTERN_WORD;
	const uint32_t returned = show_keyboard(cancel_title, cancel_description, &o);
	log_return("keyboard: cancelled", returned, &o, false);
	if (returned == ERROR_IO_PENDING && run_blocking(&wait_call, PERSON_TIMEOUT_MS))
		xsdk_log_write("%s: length 0x%08X", wait_call.what, (unsigned)waited_length);
	(void)complete("keyboard: cancelled", returned, &o, QUICK_TIMEOUT_MS, false);
	log_keyboard_text("keyboard: cancelled");
	settle_ui("keyboard: cancelled", ui_closings);
}

static void run_message_box(void)
{
	static XAM_OVERLAPPED o;
	pressed = PATTERN_WORD;
	overlapped_start(&o, true);
	const uint32_t result = finish("message_box: XamShowMessageBoxUI(2 buttons)",
		XamShowMessageBoxUI(0, box_title, box_text, BUTTONS, buttons, 0, 0, &pressed, &o), &o, PERSON_TIMEOUT_MS,
		false);
	xsdk_log_write("message_box: result 0x%08X, button 0x%08X (the second is 1)", (unsigned)result, (unsigned)pressed);
	settle_ui("message_box", ui_closings);
}

static uint32_t box_without_overlapped(void)
{
	return XamShowMessageBoxUI(0, box_title, null_box_text, BUTTONS, buttons, 0, 0, &pressed, NULL);
}

static uint32_t keyboard_without_overlapped(void)
{
	return show_keyboard(null_title, null_description, NULL);
}

// Gives XAM's UI time to say it opened, then waits for it to close.
static void settle_ui(const char *what, uint32_t closings)
{
	pause_ms(UI_SETTLE_MS);
	if (ui_open || ui_closings != closings)
		(void)wait_ui_closed(what, closings, PERSON_TIMEOUT_MS);
}

static void run_null_overlapped(void)
{
	static BlockingCall box = { .what = "null_overlapped: XamShowMessageBoxUI with no overlapped",
		.call = box_without_overlapped };
	static BlockingCall keyboard = { .what = "null_overlapped: XamShowKeyboardUI with no overlapped",
		.call = keyboard_without_overlapped };
	pressed = PATTERN_WORD;
	uint32_t closings = ui_closings;
	if (run_blocking(&box, PERSON_TIMEOUT_MS))
		xsdk_log_write("null_overlapped: button 0x%08X", (unsigned)pressed);
	settle_ui("null_overlapped: the message box", closings);
	closings = ui_closings;
	if (run_blocking(&keyboard, PERSON_TIMEOUT_MS))
		log_keyboard_text("null_overlapped: the keyboard");
	settle_ui("null_overlapped: the keyboard", closings);
}

static bool wait_ui_open(const char *what, uint32_t timeout_ms)
{
	const uint64_t start = now_us();
	while (!ui_open) {
		if (ms_since(start) >= timeout_ms) {
			xsdk_log_write("%s: XAM's UI didn't open in %u ms", what, (unsigned)timeout_ms);
			return false;
		}
		take_notifications();
		(void)xsdk_thread_sleep(POLL_MS);
	}
	return true;
}

static void run_guide_open(void)
{
	static XAM_OVERLAPPED o;
	for (uint32_t round = 0; round < GUIDE_ROUNDS; round++) {
		const char *const what =
			round ? "guide_open: XamShowKeyboardUI under the Guide" : "guide_open: XamShowMessageBoxUI under the Guide";
		xsdk_log_write("guide_open: press the Guide button now");
		if (!wait_ui_open(what, PERSON_TIMEOUT_MS))
			return;
		const uint32_t closings = ui_closings;
		pause_ms(UI_SETTLE_MS);
		xsdk_log_write("%s: the Guide is open, XamIsUIActive %u", what, (unsigned)XamIsUIActive());
		pressed = PATTERN_WORD;
		overlapped_start(&o, true);
		const uint32_t returned = round
			? show_keyboard(guide_title, guide_description, &o)
			: XamShowMessageBoxUI(0, box_title, guide_box_text, BUTTONS, buttons, 0, 0, &pressed, &o);
		xsdk_log_write("guide_open: close the Guide, then answer what shows: any button, or B on the keyboard");
		const uint32_t result = finish(what, returned, &o, PERSON_TIMEOUT_MS, false);
		xsdk_log_write("%s: result 0x%08X, button 0x%08X", what, (unsigned)result, (unsigned)pressed);
		if (round)
			log_keyboard_text(what);
		settle_ui(what, closings);
	}
}

static const ProbeCase cases[] = {
	{ "users", UNATTENDED, "sign-in states, XUIDs, the name and sign-in info", run_users },
	{ "devices", UNATTENDED, "the device enumerator, and each device's data, name and state", run_devices },
	{ "overlapped", UNATTENDED, "how overlapped calls complete", run_overlapped },
	{ "content", UNATTENDED, "a save made, written, closed, opened again and read, for users 0xFE, 0xFF and 0",
		run_content },
	{ "close_open_file", UNATTENDED, "a package closed with a file in it open", run_close_open_file },
	{ "delete", ON_REQUEST, "the probe's packages deleted", run_delete },
	{ "signed_in_content", PERSON, "content's save for user 0, a profile signed in", run_signed_in_content },
	{ "settings", PERSON, "invert Y and vibration from user 0's profile", run_settings },
	{ "signin_ui", PERSON, "the sign-in UI and what it notifies", run_signin_ui },
	{ "keyboard", PERSON, "the keyboard, accepted, then cancelled", run_keyboard },
	{ "message_box", PERSON, "a message box of 2 buttons", run_message_box },
	{ "null_overlapped", PERSON, "a message box and the keyboard with no overlapped", run_null_overlapped },
	{ "guide_open", PERSON, "a message box, then the keyboard, asked for with the Guide open", run_guide_open },
};

enum { CASE_COUNT = sizeof cases / sizeof cases[0] };

// The cases game:\xam_start.txt names, in order; without it, or naming none, the unattended ones.
static uint32_t chosen_cases(const ProbeCase *chosen[MAX_RUN])
{
	uint32_t count = 0;
	FILE *const file = fopen(start_path, "r");
	char name[CASE_NAME_BYTES];
	while (file && count < MAX_RUN && fscanf(file, "%31s", name) == 1) {
		uint32_t i = 0;
		while (i < CASE_COUNT && strcmp(name, cases[i].name) != 0)
			i++;
		if (i < CASE_COUNT)
			chosen[count++] = &cases[i];
		else
			xsdk_log_write("probe_xam: %s names no case '%s'", start_path, name);
	}
	if (file)
		(void)fclose(file);
	if (count > 0)
		return count;
	xsdk_log_write("probe_xam: no cases named in %s; running the unattended ones", start_path);
	for (uint32_t i = 0; i < CASE_COUNT; i++) {
		if (cases[i].kind == UNATTENDED)
			chosen[count++] = &cases[i];
	}
	return count;
}

static bool start_presenting(BarScene *scene, pthread_t *thread)
{
	const NTSTATUS status = xsdk_gpu_open();
	uint32_t width = 0;
	uint32_t height = 0;
	const bool opened = NT_SUCCESS(status) && bar_scene_open(scene, BAR_ARGB) &&
		NT_SUCCESS(xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &width, &height));
	if (!opened || pthread_create(thread, NULL, present_frames, scene) != 0) {
		xsdk_log_write("probe_xam: FAILED to start presenting: GPU 0x%08X, display %ux%u", (unsigned)status,
			(unsigned)width, (unsigned)height);
		return false;
	}
	return true;
}

static void wait_for_back(void)
{
	xsdk_log_write("probe_xam: done; close any dialog or the Guide, then press Back to end");
	for (;;) {
		take_notifications();
		for (uint32_t user = 0; !ui_open && user < 4; user++) {
			INPUT_STATE input;
			if (XamInputGetState(user, 0, &input) == ERROR_SUCCESS && input.gamepad.buttons & INPUT_GAMEPAD_BACK)
				return;
		}
		(void)xsdk_thread_sleep(POLL_MS);
	}
}

int main(void)
{
	start_us = now_us();
	const ProbeCase *chosen[MAX_RUN];
	const uint32_t count = chosen_cases(chosen);
	(void)xsdk_splash_show(SPLASH_MS);
	BarScene scene;
	pthread_t presenter;
	if (!start_presenting(&scene, &presenter))
		return 1;
	listener = XamNotifyCreateListener(UINT64_MAX, NOTIFY_MAX_VERSION);
	xsdk_log_write("probe_xam: %u cases; listener 0x%08X, XamIsUIActive %u", (unsigned)count, (unsigned)listener,
		(unsigned)XamIsUIActive());
	bool person = false;
	for (uint32_t i = 0; i < count; i++) {
		xsdk_log_write("probe_xam: case %s: %s; at %u ms, frame %u", chosen[i]->name, chosen[i]->what,
			ms_since(start_us), (unsigned)frames_presented);
		chosen[i]->run();
		take_notifications();
		person |= chosen[i]->kind == PERSON;
		xsdk_log_write("probe_xam: case %s done at %u ms, frame %u", chosen[i]->name, ms_since(start_us),
			(unsigned)frames_presented);
	}
	if (person || ui_open)
		wait_for_back();
	stop_presenting = true;
	(void)pthread_join(presenter, NULL);
	xsdk_present_close();
	bar_scene_close(&scene);
	const NTSTATUS status = xsdk_gpu_close();
	(void)NtClose(listener);
	xsdk_log_write(
		"probe_xam: done after %u frames; xsdk_gpu_close 0x%08X", (unsigned)frames_presented, (unsigned)status);
	return 0;
}
