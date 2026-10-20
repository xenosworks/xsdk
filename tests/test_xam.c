#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "tests.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"
#include "xsdk/xam.h"

// Milestone 51's checks that need nobody at the console (docs/scope/51-saves-and-profiles.md).

enum {
	// XamUserGetXUID's type mask 1, the first the probe tried.
	XUID_TYPE_MASK = 1,
	// The console's calls took 98 ms at most (probe_xam_t2.log).
	TIMEOUT_MS = 1000,
	POLL_MS = 1,
	TEXT_BYTES = 64,
};

// XamUserGetXUID's result with nobody signed in (results/2026-10-19-milestone-51/probe_xam.log).
static const uint32_t XUID_NO_SUCH_USER = 0x80070525;
static const char *const save_path = "save:/xsdk_test.dat";

// Static, as XAM may write into it after a call that timed out.
static XsdkSave save;

static uint32_t ms_since(const struct timespec *start)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint32_t)((now.tv_sec - start->tv_sec) * 1000 + (now.tv_nsec - start->tv_nsec) / 1000000);
}

// A console set to sign a profile in by itself, as Xenia always signs user 0 in (xenia:src/xenia/kernel/xam/
// user_profile.h:229), takes the first check; one with nobody signed in, the other two.
static void check_signed_in(void)
{
	uint32_t states = 0;
	for (uint32_t user = 0; user < XSDK_XAM_USERS; user++)
		states |= XamUserGetSigninState(user);
	uint32_t user = 0;
	char gamertag[XSDK_XAM_GAMERTAG_BYTES];
	const uint32_t result = xsdk_xam_signed_in_user(&user, gamertag);
	xsdk_log_write("xam: sign-in states OR'd 0x%X; xsdk_xam_signed_in_user 0x%08X, user 0x%X, '%s'", (unsigned)states,
		(unsigned)result, (unsigned)user, gamertag);
	if (states) {
		check("xam: the signed-in user's gamertag reads",
			result == ERROR_SUCCESS && user < XSDK_XAM_USERS && gamertag[0] != '\0');
		return;
	}
	check("xam: nobody is signed in", result == ERROR_NO_SUCH_USER && user == XSDK_XAM_USER_NONE);

	uint64_t xuid = 0;
	const uint32_t xuid_result = XamUserGetXUID(0, XUID_TYPE_MASK, &xuid);
	const uint32_t name_result = XamUserGetName(0, gamertag, sizeof gamertag);
	xsdk_log_write(
		"xam: user 0's XamUserGetXUID 0x%08X, XamUserGetName 0x%08X", (unsigned)xuid_result, (unsigned)name_result);
	check("xam: with nobody signed in, user 0's XUID and name fail as on the console",
		xuid_result == XUID_NO_SUCH_USER && name_result == ERROR_NO_SUCH_USER);
}

static void check_device(void)
{
	struct timespec start;
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	uint32_t device = 0;
	const uint32_t result = xsdk_save_device(&device);
	const uint32_t took_ms = ms_since(&start);
	CONTENT_DEVICE_DATA data = { 0 };
	const uint32_t data_result = result == ERROR_SUCCESS ? XamContentGetDeviceData(device, &data) : result;
	xsdk_log_write("xam: xsdk_save_device 0x%08X in %u ms, device 0x%X; its data 0x%08X, type 0x%X, %llu bytes free",
		(unsigned)result, (unsigned)took_ms, (unsigned)device, (unsigned)data_result, (unsigned)data.device_type,
		(unsigned long long)data.free_bytes);
	check("xam: the device enumerator finds the hard disk", result == ERROR_SUCCESS && data_result == ERROR_SUCCESS);
}

// XamContentGetDeviceState is still running straight after it returns on the console (probe_xam.log), so only a wait
// on its event finds it done; Xenia completes it before it returns (xam_content_device.cc:78-98).
static void check_event(void)
{
	uint32_t device = 0;
	if (xsdk_save_device(&device) != ERROR_SUCCESS) {
		check("xam: an overlapped call completes through its event", false);
		return;
	}
	static XAM_OVERLAPPED overlapped;
	xsdk_xam_prepare(&overlapped);
	const uint32_t returned = xsdk_xam_returned(&overlapped, XamContentGetDeviceState(device, &overlapped));
	const bool had_event = overlapped.event != 0;
	const uint32_t result = returned == ERROR_IO_PENDING ? xsdk_xam_wait(&overlapped, TIMEOUT_MS, NULL) : returned;
	xsdk_log_write("xam: XamContentGetDeviceState(0x%X) returned 0x%08X, then 0x%08X; event closed %d",
		(unsigned)device, (unsigned)returned, (unsigned)result, overlapped.event == 0);
	check("xam: an overlapped call completes through its event",
		had_event && returned == ERROR_IO_PENDING && result == ERROR_SUCCESS && overlapped.event == 0);
}

// Polls as a frame loop would, every millisecond for up to a second.
static uint32_t finish(const char *what, uint32_t returned)
{
	struct timespec start;
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	uint32_t result = returned;
	while (result == ERROR_IO_PENDING || result == ERROR_IO_INCOMPLETE) {
		result = xsdk_save_wait(&save, 0);
		if (result != ERROR_IO_INCOMPLETE || ms_since(&start) >= TIMEOUT_MS)
			break;
		(void)xsdk_thread_sleep(POLL_MS);
	}
	xsdk_log_write("xam: %s returned 0x%08X, then 0x%08X after %u ms", what, (unsigned)returned, (unsigned)result,
		(unsigned)ms_since(&start));
	return result;
}

static uint32_t mount(void)
{
	return finish(
		"xsdk_save_mount", xsdk_save_mount(&save, XSDK_XAM_USER_NONE, "save", "xsdk_test", u"xsdk_test's save check"));
}

static bool write_text(const char *text)
{
	FILE *const file = fopen(save_path, "wb");
	if (!file)
		return false;
	const bool wrote = fwrite(text, 1, strlen(text), file) == strlen(text);
	return fclose(file) == 0 && wrote;
}

static bool reads_back(const char *text)
{
	FILE *const file = fopen(save_path, "rb");
	if (!file)
		return false;
	char read[TEXT_BYTES] = "";
	const size_t length = fread(read, 1, sizeof read - 1, file);
	(void)fclose(file);
	return length == strlen(text) && memcmp(read, text, length) == 0;
}

// Written, unmounted, mounted again, read back and deleted, so runs leave no package behind.
static void check_save(void)
{
	uint64_t now = 0;
	KeQuerySystemTime(&now);
	char text[TEXT_BYTES];
	snprintf(text, sizeof text, "xsdk_test's save at %llu", (unsigned long long)now);

	const bool mounted = mount() == ERROR_SUCCESS;
	const bool written = mounted && write_text(text);
	const bool unmounted = mounted && finish("xsdk_save_unmount", xsdk_save_unmount(&save)) == ERROR_SUCCESS;
	xsdk_log_write("xam: the save's flush 0x%08X", (unsigned)save.flush_result);
	check("xam: a save no profile owns mounts, takes a file and unmounts", mounted && written && unmounted);
	check("xam: a save is flushed before it's unmounted", unmounted && save.flush_result == ERROR_SUCCESS);
	if (!unmounted)
		return;

	const bool again = mount() == ERROR_SUCCESS;
	const uint32_t disposition = save.disposition;
	const bool read = again && reads_back(text);
	const bool closed = again && finish("xsdk_save_unmount", xsdk_save_unmount(&save)) == ERROR_SUCCESS;
	xsdk_log_write("xam: mounted again with disposition %u", (unsigned)disposition);
	check("xam: a save mounted again opens the package and reads back what was written",
		again && disposition == XSDK_SAVE_OPENED && read && closed);
	if (again && !closed)
		return;

	xsdk_xam_prepare(&save.overlapped);
	const uint32_t deleted = finish("XamContentDelete",
		xsdk_xam_returned(&save.overlapped, XamContentDelete(save.user_index, &save.data, &save.overlapped)));
	check("xam: the save is deleted", deleted == ERROR_SUCCESS);
}

void test_xam(void)
{
	check_signed_in();
	check_device();
	check_event();
	check_save();
}
