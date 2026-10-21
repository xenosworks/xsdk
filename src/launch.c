#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "exit_private.h"
#include "file_private.h"
#include "path_private.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/xam.h"

// Launching another .xex, and the data a title was launched with. XAM takes drives such as game: but not device paths
// (docs/HARDWARE.md, "XAM's sockets and loader, milestone 56"), so a .xex in the title's own folder goes as game:, and
// one elsewhere through a drive of the title's own linked to its folder.

static const char LINK_NAME[] = "\\??\\launch:";
static const char LINK_DRIVE[] = "launch:";

static uint8_t launch_data[XSDK_LAUNCH_DATA_MAX_BYTES];
static uint32_t launch_data_bytes;
static pthread_once_t launch_data_once = PTHREAD_ONCE_INIT;

// XAM has none left once it's read (docs/HARDWARE.md), so it's read once and kept.
static void read_launch_data(void)
{
	uint32_t size = 0;
	if (XamLoaderGetLaunchDataSize(&size) != ERROR_SUCCESS || size == 0)
		return;
	if (size > sizeof(launch_data))
		size = sizeof(launch_data);
	const uint32_t result = XamLoaderGetLaunchData(launch_data, size);
	if (result != ERROR_SUCCESS) {
		xsdk_log_write("launch: XamLoaderGetLaunchData failed: 0x%08X", (unsigned)result);
		return;
	}
	launch_data_bytes = size;
}

size_t xsdk_launch_data(void *buffer, size_t size_bytes)
{
	(void)pthread_once(&launch_data_once, read_launch_data);
	memcpy(buffer, launch_data, size_bytes < launch_data_bytes ? size_bytes : launch_data_bytes);
	return launch_data_bytes;
}

static ANSI_STRING ansi_string(const char *text)
{
	const size_t length = strlen(text);
	return (ANSI_STRING){ .length = (uint16_t)length, .maximum_length = (uint16_t)(length + 1), .pointer = text };
}

// Links LINK_NAME to folder, in place of a link left from before.
static int link_folder(const char *folder)
{
	const ANSI_STRING link = ansi_string(LINK_NAME);
	const ANSI_STRING target = ansi_string(folder);
	(void)ObDeleteSymbolicLink(&link);
	const NTSTATUS status = ObCreateSymbolicLink(&link, &target);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("launch: ObCreateSymbolicLink(%s, %s) failed: 0x%08X", LINK_NAME, folder, (unsigned)status);
		return xsdk_file_errno(status);
	}
	return 0;
}

// The path XAM takes for a resolved one; returns 0 or an errno.
static int xam_path(const char *resolved, char xam[XSDK_PATH_BYTES])
{
	if (resolved[0] != '\\') {
		strcpy(xam, resolved);
		return 0;
	}
	const char *const image = ExLoadedImageName ? ExLoadedImageName : "";
	const char *const image_name = strrchr(image, '\\');
	const size_t own_bytes = image_name ? (size_t)(image_name - image) + 1 : 0;
	if (own_bytes && strncasecmp(resolved, image, own_bytes) == 0)
		return snprintf(xam, XSDK_PATH_BYTES, "game:\\%s", resolved + own_bytes) < XSDK_PATH_BYTES ? 0 : ENAMETOOLONG;
	const char *const name = strrchr(resolved, '\\');
	char folder[XSDK_PATH_BYTES];
	(void)snprintf(folder, sizeof folder, "%.*s", (int)(name - resolved), resolved);
	const int error = link_folder(folder);
	if (error)
		return error;
	(void)snprintf(xam, XSDK_PATH_BYTES, "%s%s", LINK_DRIVE, name);
	return 0;
}

int xsdk_launch(const char *path, const void *data, size_t size_bytes)
{
	if (size_bytes > XSDK_LAUNCH_DATA_MAX_BYTES) {
		errno = E2BIG;
		return -1;
	}
	char resolved[XSDK_PATH_BYTES];
	int error = xsdk_path_resolve(AT_FDCWD, path, resolved);
	if (error) {
		errno = error;
		return -1;
	}
	// XAM ends the title for a path to nothing (docs/HARDWARE.md), so that fails here first.
	struct stat info;
	if (stat(resolved, &info) != 0)
		return -1;
	if (S_ISDIR(info.st_mode)) {
		errno = EISDIR;
		return -1;
	}
	char xam[XSDK_PATH_BYTES];
	error = xam_path(resolved, xam);
	if (error) {
		errno = error;
		return -1;
	}
	if (size_bytes) {
		const uint32_t result = XamLoaderSetLaunchData(data, (uint32_t)size_bytes);
		if (result != ERROR_SUCCESS) {
			xsdk_log_write("launch: XamLoaderSetLaunchData failed: 0x%08X", (unsigned)result);
			errno = EIO;
			return -1;
		}
	}
	xsdk_exit_launching(xam);
}
