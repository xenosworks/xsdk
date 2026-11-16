#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"
#include "xsdk/thread.h"

// Milestone 78 step 1 (docs/scope/78-exfat-hdd.md), the sign-in that spins after a profile is created on the exFAT
// drive. The dash signs the new profile in with XamUserLogon (xam ordinal 523, resolved at run time as the SDK does not
// import it) given a 392-byte record (the XUID, the 380-byte account, the device id, 0 from the dash) and flags 0x24,
// and its overlapped stays at ERROR_IO_PENDING: the logon task mounts the profile's package, reads its header and hash
// tables and closes it through the error path (module trace, run 25). This makes the same call from a title for the
// newest profile xam lists, so the module's stick trace (usb0:\XSDK\exfat\exfat_trace.log) shows whether the mount
// fails the same way long after the creation, and logs what xam answers, with the devices and profiles first. Nothing
// is written by the probe itself; a logon writes the profile's package.

enum { SPLASH_MS = 1500, ITEMS = 32, POLL_MS = 100, WAIT_MS = 10000, PROFILE = 0x10000, ANY_USER = 0xFF };
enum { XAM_USER_LOGON = 523, DASH_LOGON_FLAGS = 0x24, RECORDS = 4 };

typedef uint32_t (*UserLogon)(const void *records, uint32_t flags, XAM_OVERLAPPED *overlapped);

static void log_devices(void)
{
	static CONTENT_DEVICE_DATA devices[ITEMS];
	uint32_t bytes = 0;
	HANDLE enumerator = 0;
	uint32_t result = XamContentCreateDeviceEnumerator(PROFILE, 0, ITEMS, &bytes, &enumerator);
	xsdk_log_write("RESULT device enumerator = %08X", (unsigned)result);
	if (result != ERROR_SUCCESS)
		return;
	uint32_t count = 0;
	result = XamEnumerate(enumerator, 0, devices, sizeof devices, &count, NULL);
	xsdk_log_write("RESULT devices = %08X, %u devices", (unsigned)result, (unsigned)count);
	for (uint32_t i = 0; i < count && i < ITEMS; i++) {
		char name[29];
		for (unsigned c = 0; c < 28; c++)
			name[c] = devices[i].name[c] < 128 ? (char)devices[i].name[c] : '?';
		name[28] = 0;
		xsdk_log_write("RESULT device %u type %u \"%s\" %llu MB, %llu MB free, state %08X",
			(unsigned)devices[i].device_id, (unsigned)devices[i].device_type, name,
			(unsigned long long)(devices[i].total_bytes >> 20), (unsigned long long)(devices[i].free_bytes >> 20),
			(unsigned)XamContentGetDeviceState(devices[i].device_id, NULL));
	}
	(void)NtClose(enumerator);
}

// The record of the profile with the highest XUID among those xam lists on every device, copied into record; 0 without
// one. The content enumerator lists the calling title's content only, and profiles belong to the dash (FFFE07D1), so
// the profile enumerator (xam ordinals 561 and 562, sub_8199E3E0 takes the same 392-byte records) is used: an offline
// XUID, the 380-byte account, the device.
typedef uint32_t (*ProfileCreateEnumerator)(uint32_t device_id, HANDLE *enumerator);
typedef uint32_t (*ProfileEnumerate)(HANDLE enumerator, uint32_t flags, void *record, XAM_OVERLAPPED *overlapped);
enum {
	XAM_PROFILE_CREATE_ENUMERATOR = 561,
	XAM_PROFILE_ENUMERATE = 562,
	RECORD_BYTES = 392,
	GAMERTAG = 16,
	DEVICE = 388
};

static void *xam_export(HANDLE xam, uint32_t ordinal)
{
	void *address = NULL;
	(void)XexGetProcedureAddress(xam, ordinal, &address);
	return address;
}

static uint64_t newest_profile(HANDLE xam, uint8_t *newest_record)
{
	const ProfileCreateEnumerator create = (ProfileCreateEnumerator)xam_export(xam, XAM_PROFILE_CREATE_ENUMERATOR);
	const ProfileEnumerate enumerate = (ProfileEnumerate)xam_export(xam, XAM_PROFILE_ENUMERATE);
	if (!create || !enumerate) {
		xsdk_log_write("RESULT no profile enumerator exports");
		return 0;
	}
	HANDLE enumerator = 0;
	uint32_t result = create(0, &enumerator);
	xsdk_log_write("RESULT profile enumerator = %08X", (unsigned)result);
	if (result != ERROR_SUCCESS)
		return 0;
	uint64_t newest = 0;
	for (unsigned n = 0; n < ITEMS; n++) {
		static uint8_t record[RECORD_BYTES];
		memset(record, 0, sizeof record);
		result = enumerate(enumerator, 0, record, NULL);
		if (result != ERROR_SUCCESS) {
			xsdk_log_write("RESULT profiles = %08X after %u", (unsigned)result, n);
			break;
		}
		uint64_t xuid;
		uint32_t device;
		memcpy(&xuid, record, sizeof xuid);
		memcpy(&device, record + DEVICE, sizeof device);
		char tag[GAMERTAG + 1];
		for (unsigned c = 0; c < GAMERTAG; c++) {
			const uint16_t w = (uint16_t)(record[GAMERTAG + 2 * c] << 8 | record[GAMERTAG + 2 * c + 1]);
			tag[c] = w && w < 128 ? (char)w : w ? '?' : 0;
		}
		tag[GAMERTAG] = 0;
		xsdk_log_write("RESULT profile %08X%08X on device %u, \"%s\", flags %02X%02X%02X%02X %02X%02X%02X%02X",
			(unsigned)(xuid >> 32), (unsigned)xuid, (unsigned)device, tag, record[8], record[9], record[10], record[11],
			record[12], record[13], record[14], record[15]);
		if (xuid > newest) {
			newest = xuid;
			memcpy(newest_record, record, RECORD_BYTES);
		}
	}
	(void)NtClose(enumerator);
	return newest;
}

static void log_user(const char *when)
{
	uint64_t xuid = 0;
	const uint32_t got = XamUserGetXUID(0, 3, &xuid);
	char name[16] = "";
	(void)XamUserGetName(0, name, sizeof name);
	xsdk_log_write("RESULT %s: user 0 state %u, XamUserGetXUID = %08X, xuid %08X%08X, name \"%.15s\"", when,
		(unsigned)XamUserGetSigninState(0), (unsigned)got, (unsigned)(xuid >> 32), (unsigned)xuid, name);
}

int main(void)
{
	xsdk_log_write("probe_signin: kernel %u.%u.%u.%u", XboxKrnlVersion->major, XboxKrnlVersion->minor,
		XboxKrnlVersion->build, XboxKrnlVersion->qfe);
	(void)xsdk_splash_show(SPLASH_MS);

	HANDLE xam;
	void *logon = NULL;
	if (XexGetModuleHandle("xam.xex", &xam) != 0 || XexGetProcedureAddress(xam, XAM_USER_LOGON, &logon) != 0 ||
		!logon) {
		xsdk_log_write("RESULT no XamUserLogon");
		return 0;
	}
	log_devices();
	static uint8_t records[RECORDS * RECORD_BYTES];
	const uint64_t xuid = newest_profile(xam, records);
	log_user("before");
	if (!xuid) {
		xsdk_log_write("RESULT no profile to sign in");
		return 0;
	}

	// The dash's record carries the account and device 0.
	memset(records + DEVICE, 0, 4);
	static XAM_OVERLAPPED overlapped;
	const uint32_t result = ((UserLogon)logon)(records, DASH_LOGON_FLAGS, &overlapped);
	xsdk_log_write("RESULT XamUserLogon(record %08X%08X device 0, flags %02X, overlapped) = %08X, status %08X",
		(unsigned)(xuid >> 32), (unsigned)xuid, (unsigned)DASH_LOGON_FLAGS, (unsigned)result,
		(unsigned)overlapped.result);
	uint32_t waited = 0;
	for (; waited < WAIT_MS && overlapped.result == ERROR_IO_PENDING; waited += POLL_MS)
		(void)xsdk_thread_sleep(POLL_MS);
	uint32_t length = 0;
	const uint32_t got = XamGetOverlappedResult(&overlapped, &length, 0);
	xsdk_log_write(
		"RESULT after %u ms: overlapped result %08X, extended error %08X, length %u, XamGetOverlappedResult %08X",
		(unsigned)waited, (unsigned)overlapped.result, (unsigned)overlapped.extended_error, (unsigned)length,
		(unsigned)got);
	log_user("after");
	if (overlapped.result == ERROR_IO_PENDING)
		xsdk_log_write("RESULT the logon is still pending; the title leaves with it pending");
	return 0;
}
