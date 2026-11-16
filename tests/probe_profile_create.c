#include <stdint.h>
#include <string.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"
#include "xsdk/thread.h"

// Milestone 78 step 1 (docs/scope/78-exfat-hdd.md), the sign-in that spins after a profile is created on the exFAT
// drive: the dash polls XamProfileGetCreationStatus and never calls XamUserLogon, so the creation never reports done.
// This creates a profile from a title the way the dash does, XamProfileCreate (xam ordinal 560: flags, the device id's
// address, the XUID or 0, the 380-byte account, three optional blobs, the request out) with an account copied from the
// newest profile xam lists, then polls XamProfileGetCreationStatus (564) and reads the task xam keeps at the request's
// +2744 (flags +8, state +16, +24, the routine's result +56; sub_816B7B98 reports done at state 2 or 64) until it
// completes or 15 s pass. A profile is written to the drive; nothing else.

enum { SPLASH_MS = 1500, ITEMS = 32, POLL_MS = 200, WAIT_MS = 15000, HARD_DRIVE = 1 };
enum {
	XAM_PROFILE_CREATE = 560,
	XAM_PROFILE_CREATE_ENUMERATOR = 561,
	XAM_PROFILE_ENUMERATE = 562,
	XAM_PROFILE_GET_CREATION_STATUS = 564
};
enum { RECORD_BYTES = 392, ACCOUNT = 8, ACCOUNT_BYTES = 380, GAMERTAG = 16, GAMERTAG_CHARS = 16 };
enum { REQUEST_XUID = 2680, REQUEST_TASK = 2744, REQUEST_FLAG = 2748 };

typedef uint32_t (*ProfileCreateEnumerator)(uint32_t device_id, HANDLE *enumerator);
typedef uint32_t (*ProfileEnumerate)(HANDLE enumerator, uint32_t flags, void *record, XAM_OVERLAPPED *overlapped);
typedef uint32_t (*ProfileCreate)(uint32_t flags, uint32_t *device_id, uint64_t xuid, const void *account,
	const void *settings, const void *a, const void *b, void **request);
typedef uint32_t (*CreationStatus)(void *request, uint64_t *xuid);

static void *xam_export(HANDLE xam, uint32_t ordinal)
{
	void *address = NULL;
	(void)XexGetProcedureAddress(xam, ordinal, &address);
	return address;
}

static void tag_of(const uint8_t *account, char *tag)
{
	for (unsigned c = 0; c < GAMERTAG_CHARS; c++) {
		const unsigned w = (unsigned)account[GAMERTAG - ACCOUNT + 2 * c] << 8 | account[GAMERTAG - ACCOUNT + 2 * c + 1];
		tag[c] = w == 0 ? 0 : w < 128 ? (char)w : '?';
	}
	tag[GAMERTAG_CHARS] = 0;
}

// The account of the newest profile xam lists, copied into account; false without one.
static bool newest_account(HANDLE xam, uint8_t *account)
{
	const ProfileCreateEnumerator create = (ProfileCreateEnumerator)xam_export(xam, XAM_PROFILE_CREATE_ENUMERATOR);
	const ProfileEnumerate enumerate = (ProfileEnumerate)xam_export(xam, XAM_PROFILE_ENUMERATE);
	HANDLE enumerator = 0;
	if (!create || !enumerate || create(0, &enumerator) != ERROR_SUCCESS) {
		xsdk_log_write("RESULT no profile enumerator");
		return false;
	}
	uint64_t newest = 0;
	for (unsigned n = 0; n < ITEMS; n++) {
		static uint8_t record[RECORD_BYTES];
		memset(record, 0, sizeof record);
		if (enumerate(enumerator, 0, record, NULL) != ERROR_SUCCESS)
			break;
		uint64_t xuid;
		uint32_t device;
		memcpy(&xuid, record, sizeof xuid);
		memcpy(&device, record + RECORD_BYTES - 4, sizeof device);
		char tag[GAMERTAG_CHARS + 1];
		tag_of(record + ACCOUNT, tag);
		xsdk_log_write("RESULT profile %08X%08X on device %u \"%s\"", (unsigned)(xuid >> 32), (unsigned)xuid,
			(unsigned)device, tag);
		if (xuid > newest) {
			newest = xuid;
			memcpy(account, record + ACCOUNT, ACCOUNT_BYTES);
		}
	}
	(void)NtClose(enumerator);
	return newest != 0;
}

static void log_task(const char *when, const uint8_t *request)
{
	const uint32_t task = *(const uint32_t *)(request + REQUEST_TASK) & ~1u;
	const uint64_t xuid = *(const uint64_t *)(request + REQUEST_XUID);
	if (!task || !MmIsAddressValid((const void *)(uintptr_t)task)) {
		xsdk_log_write("RESULT %s: request %08X flags %08X, task %08X unreadable", when, (unsigned)(uintptr_t)request,
			(unsigned)*(const uint32_t *)request, (unsigned)task);
		return;
	}
	const uint32_t *const t = (const uint32_t *)(uintptr_t)task;
	xsdk_log_write(
		"RESULT %s: request %08X flags %08X xuid %08X%08X; task %08X: +8 %08X +12 %08X +16 state %08X +24 %08X "
		"+56 result %08X",
		when, (unsigned)(uintptr_t)request, (unsigned)*(const uint32_t *)request, (unsigned)(xuid >> 32),
		(unsigned)xuid, (unsigned)task, (unsigned)t[2], (unsigned)t[3], (unsigned)t[4], (unsigned)t[6],
		(unsigned)t[14]);
}

int main(void)
{
	xsdk_log_write("probe_profile_create: kernel %u.%u.%u.%u", XboxKrnlVersion->major, XboxKrnlVersion->minor,
		XboxKrnlVersion->build, XboxKrnlVersion->qfe);
	(void)xsdk_splash_show(SPLASH_MS);

	HANDLE xam;
	if (XexGetModuleHandle("xam.xex", &xam) != 0) {
		xsdk_log_write("RESULT no xam");
		return 0;
	}
	const ProfileCreate create = (ProfileCreate)xam_export(xam, XAM_PROFILE_CREATE);
	const CreationStatus status_of = (CreationStatus)xam_export(xam, XAM_PROFILE_GET_CREATION_STATUS);
	static uint8_t account[ACCOUNT_BYTES];
	if (!create || !status_of || !newest_account(xam, account)) {
		xsdk_log_write("RESULT no exports or no account to copy");
		return 0;
	}
	static const uint16_t probe_tag[GAMERTAG_CHARS] = { 'P', 'r', 'o', 'b', 'e', '1' };
	for (unsigned c = 0; c < GAMERTAG_CHARS; c++) {
		account[GAMERTAG - ACCOUNT + 2 * c] = (uint8_t)(probe_tag[c] >> 8);
		account[GAMERTAG - ACCOUNT + 2 * c + 1] = (uint8_t)probe_tag[c];
	}
	memset(account + 24, 0, 8); // the online XUID
	char tag[GAMERTAG_CHARS + 1];
	tag_of(account, tag);
	xsdk_log_write("RESULT account flags %02X%02X%02X%02X %02X%02X%02X%02X, gamertag \"%s\"", account[0], account[1],
		account[2], account[3], account[4], account[5], account[6], account[7], tag);

	static uint32_t device = HARD_DRIVE;
	void *request = NULL;
	const uint32_t created = create(1, &device, 0, account, NULL, NULL, NULL, &request);
	xsdk_log_write("RESULT XamProfileCreate(1, device %u, xuid 0, account) = %08X, request %08X", (unsigned)device,
		(unsigned)created, (unsigned)(uintptr_t)request);
	if (created & 0x80000000u || !request)
		return 0;
	const uint8_t *const req = (const uint8_t *)request;
	log_task("created", req);
	uint32_t last_status = ~0u, last_state = ~0u, waited = 0;
	for (; waited < WAIT_MS; waited += POLL_MS) {
		uint64_t xuid = 0;
		const uint32_t status = status_of(request, &xuid);
		if (status == 0) {
			xsdk_log_write("RESULT after %u ms: XamProfileGetCreationStatus = 0, xuid %08X%08X; the request is freed",
				(unsigned)waited, (unsigned)(xuid >> 32), (unsigned)xuid);
			return 0;
		}
		const uint32_t task = *(const uint32_t *)(req + REQUEST_TASK) & ~1u;
		const uint32_t state =
			task && MmIsAddressValid((const void *)(uintptr_t)task) ? *(const uint32_t *)(uintptr_t)(task + 16) : ~0u;
		if (status != last_status || state != last_state) {
			xsdk_log_write(
				"RESULT after %u ms: XamProfileGetCreationStatus = %08X", (unsigned)waited, (unsigned)status);
			log_task("  task", req);
			last_status = status;
			last_state = state;
		}
		(void)xsdk_thread_sleep(POLL_MS);
	}
	xsdk_log_write("RESULT still pending after %u ms; the title leaves with the request pending", (unsigned)waited);
	log_task("pending", req);
	return 0;
}
