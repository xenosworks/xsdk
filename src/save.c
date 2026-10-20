#include "xsdk/xam.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"

// A package is a file named file_name; with no profile it lands in the hard disk's Content\0000000000000000\<title
// ID>\00000001, a folder for each title ID (results/2026-10-19-milestone-51/probe_xam.log and probe_xam_t2.log).

enum {
	// XContentType's saved game (xenia:src/xenia/xbox.h:348-349).
	CONTENT_SAVED_GAME = 1,
	// XamContentCreateEx's open always: made if it isn't there (xenia:src/xenia/kernel/xam/xam_content.cc:173-180).
	CONTENT_OPEN_ALWAYS = 4,
	// The hard disk's type (xenia:src/xenia/kernel/xam/xam_content_device.h:19-22), the console's too (probe_xam.log).
	DEVICE_TYPE_HDD = 1,
	MAX_DEVICES = 8,
};

// 0 until found: the enumerators gave 1 and 3 on the console (probe_xam.log), 1 and 2 in Xenia.
static uint32_t hard_disk;

static uint32_t hard_disk_in(const CONTENT_DEVICE_DATA *devices, uint32_t count)
{
	for (uint32_t i = 0; i < count && i < MAX_DEVICES; i++) {
		if (devices[i].device_type == DEVICE_TYPE_HDD)
			return devices[i].device_id;
	}
	return 0;
}

uint32_t xsdk_save_device(uint32_t *device_id)
{
	*device_id = hard_disk;
	if (hard_disk)
		return ERROR_SUCCESS;
	uint32_t bytes = 0;
	HANDLE enumerator = 0;
	uint32_t result = XamContentCreateDeviceEnumerator(CONTENT_SAVED_GAME, 0, MAX_DEVICES, &bytes, &enumerator);
	if (result != ERROR_SUCCESS) {
		xsdk_log_write("save: XamContentCreateDeviceEnumerator failed: 0x%08X", (unsigned)result);
		return result;
	}
	CONTENT_DEVICE_DATA devices[MAX_DEVICES];
	uint32_t count = 0;
	// Each pass gives the next devices; past the last, an error, 0x103 on the console (probe_xam.log).
	while (!hard_disk && XamEnumerate(enumerator, 0, devices, sizeof devices, &count, NULL) == ERROR_SUCCESS && count)
		hard_disk = hard_disk_in(devices, count);
	(void)NtClose(enumerator);
	*device_id = hard_disk;
	return hard_disk ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

uint32_t xsdk_save_mount(
	XsdkSave *save, uint32_t user_index, const char *drive, const char *file_name, const uint16_t *display_name)
{
	uint32_t device_id;
	const uint32_t found = xsdk_save_device(&device_id);
	if (found != ERROR_SUCCESS)
		return found;
	if (strlen(drive) >= sizeof save->drive || strlen(file_name) >= sizeof save->data.file_name)
		return ERROR_INVALID_PARAMETER;
	*save = (XsdkSave){ .user_index = user_index,
		.flush_result = ERROR_IO_PENDING,
		.data = { .device_id = device_id, .content_type = CONTENT_SAVED_GAME } };
	strcpy(save->drive, drive);
	strcpy(save->data.file_name, file_name);
	const size_t name_chars = sizeof save->data.display_name / sizeof save->data.display_name[0];
	for (size_t i = 0; i + 1 < name_chars && display_name[i]; i++)
		save->data.display_name[i] = display_name[i];
	xsdk_xam_prepare(&save->overlapped);
	return xsdk_xam_returned(&save->overlapped,
		XamContentCreateEx(user_index, save->drive, &save->data, CONTENT_OPEN_ALWAYS, &save->disposition,
			&save->license_mask, 0, 0, &save->overlapped));
}

// Files still open in the package fail with 0xC000026E once it closes (probe_xam.log).
static uint32_t start_close(XsdkSave *save)
{
	xsdk_xam_prepare(&save->overlapped);
	return xsdk_xam_returned(&save->overlapped, XamContentClose(save->drive, &save->overlapped));
}

// A close alone kept what was written on the console (probe_xam.log); flushing first still says in flush_result
// whether XAM committed the package.
uint32_t xsdk_save_unmount(XsdkSave *save)
{
	save->flush_result = ERROR_IO_PENDING;
	xsdk_xam_prepare(&save->overlapped);
	const uint32_t returned = xsdk_xam_returned(&save->overlapped, XamContentFlush(save->drive, &save->overlapped));
	save->flushing = returned == ERROR_IO_PENDING;
	if (save->flushing)
		return returned;
	save->flush_result = returned;
	return start_close(save);
}

uint32_t xsdk_save_wait(XsdkSave *save, uint32_t timeout_ms)
{
	const uint32_t result = xsdk_xam_wait(&save->overlapped, timeout_ms, NULL);
	if (result == ERROR_IO_INCOMPLETE || !save->flushing)
		return result;
	save->flushing = false;
	save->flush_result = result;
	const uint32_t returned = start_close(save);
	return returned == ERROR_IO_PENDING ? xsdk_xam_wait(&save->overlapped, timeout_ms, NULL) : returned;
}
