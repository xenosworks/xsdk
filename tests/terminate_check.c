#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"
#include "xsdk/terminate.h"
#include "xsdk/thread.h"

// Milestone 54 step 4's check (docs/scope/54-porters-files-and-system.md): xsdk_on_terminate's routine runs when the
// title is ended from outside, by Xbox Home or by the console agent sending the console to the dashboard, and the
// console carries on. game:\terminate_case.txt names the case; without it, "read" runs.
// "run" removes the last run's marker, registers a routine that would write the wrong one and replaces it with one that
// writes the right one, \Device\Harddisk0\Partition1\terminate_check.txt, through NtCreateFile and NtWriteFile alone,
// then waits until the title is ended. "read", run after, checks the marker holds the right line, then removes it.

enum {
	SPLASH_MS = 2000,
	WAIT_MS = 1000,
	NAME_BYTES = 16,
	MARKER_BYTES = 64,
};

static const char *const case_path = "game:\\terminate_case.txt";
static const char marker_path[] = "\\Device\\Harddisk0\\Partition1\\terminate_check.txt";
static const char marker[] = "xsdk_on_terminate's routine ran";
static const char replaced_marker[] = "the routine xsdk_on_terminate replaced ran";

// Writes the marker through the kernel alone: the routine has no game:, stdio or newlib (include/xsdk/terminate.h).
static void write_marker(const char *text, uint32_t length_bytes)
{
	const ANSI_STRING name = {
		.length = sizeof(marker_path) - 1, .maximum_length = sizeof(marker_path), .pointer = marker_path
	};
	const OBJECT_ATTRIBUTES attributes = { .root_directory = 0, .name = &name };
	IO_STATUS_BLOCK io_status;
	HANDLE handle;
	const NTSTATUS status = NtCreateFile(&handle, GENERIC_WRITE | SYNCHRONIZE, &attributes, &io_status, NULL,
		FILE_ATTRIBUTE_NORMAL, 0, FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
	if (!NT_SUCCESS(status))
		return;
	(void)NtWriteFile(handle, 0, NULL, NULL, &io_status, text, length_bytes, NULL);
	(void)NtClose(handle);
}

static void on_terminate(void)
{
	write_marker(marker, sizeof(marker) - 1);
}

static void on_terminate_replaced(void)
{
	write_marker(replaced_marker, sizeof(replaced_marker) - 1);
}

_Noreturn static void run_case(void)
{
	if (unlink(marker_path) != 0 && errno != ENOENT)
		xsdk_log_write("terminate check: removing the last marker failed: errno %d", errno);
	xsdk_on_terminate(on_terminate_replaced);
	xsdk_on_terminate(on_terminate);
	xsdk_log_write("terminate check: routine registered; end the title from outside, then run the read case");
	for (;;)
		(void)xsdk_thread_sleep(WAIT_MS);
}

static int read_case(void)
{
	char text[MARKER_BYTES] = "";
	FILE *const file = fopen(marker_path, "r");
	if (file) {
		if (!fgets(text, sizeof(text), file))
			text[0] = '\0';
		(void)fclose(file);
	}
	xsdk_log_write("terminate check: %s: %s \"%s\"", marker_path, file ? "holds" : "isn't there, so", text);
	xsdk_log_write("%s: terminate check: the routine ran as the title was ended from outside", file ? "PASS" : "FAIL");
	xsdk_log_write("%s: terminate check: the routine that ran is the one registered last",
		strcmp(text, marker) == 0 ? "PASS" : "FAIL");
	if (file && unlink(marker_path) != 0)
		xsdk_log_write("terminate check: removing the marker failed: errno %d", errno);
	return 0;
}

int main(void)
{
	(void)xsdk_splash_show(SPLASH_MS);
	FILE *const file = fopen(case_path, "r");
	char name[NAME_BYTES] = "";
	const bool named = file && fscanf(file, "%15s", name) == 1;
	if (file)
		(void)fclose(file);
	const bool run = named && strcmp(name, "run") == 0;
	xsdk_log_write("terminate check: case %s", run ? "run" : "read");
	if (run)
		run_case();
	return read_case();
}
