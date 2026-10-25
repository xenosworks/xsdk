#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/xam.h"

// Step 1 of docs/scope/63-homebrew-store.md: whether a running title can replace its own .xex, as the store must to
// update itself. One ELF is packed twice, named "xsdk probe_replace 1" and "2". Version 1, started with version 2
// beside it as probe_replace.new, renames that over its own .xex while running from it, and launches it. A start's log
// replaces the last one, so version 1 keeps what it found in probe_replace.txt, and version 2 logs it again.

enum { LINE_BYTES = 256 };

static const char NEW[] = "probe_replace.new";
static const char OLD[] = "probe_replace.old";
static const char NOTES[] = "probe_replace.txt";
// The packer files the name under the title ID, here its default (tools/elf2xex.py).
static const char TITLE_ID_HEX[] = "5853444B";

static FILE *notes;

// Logs a line, and keeps it for version 2.
static void note(const char *format, ...)
{
	char line[LINE_BYTES];
	va_list arguments;
	va_start(arguments, format);
	(void)vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	xsdk_log_write("%s", line);
	if (notes)
		(void)fprintf(notes, "%s\n", line);
}

static long file_size(const char *path)
{
	struct stat status;
	return stat(path, &status) == 0 ? (long)status.st_size : -1;
}

// 1 or 2 from the name the loaded module carries, 0 if it carries neither.
static int running_version(void)
{
	HANDLE module = 0;
	const void *data = NULL;
	uint32_t size = 0;
	if (XexGetModuleHandle(NULL, &module) != 0 || !NT_SUCCESS(XexGetModuleSection(module, TITLE_ID_HEX, &data, &size)))
		return 0;
	if (memmem(data, size, "probe_replace 2", 15))
		return 2;
	return memmem(data, size, "probe_replace 1", 15) ? 1 : 0;
}

static void opens(const char *path, const char *mode, const char *what)
{
	FILE *const file = fopen(path, mode);
	note("probe_replace: the running .xex opened %s: %s, errno %d", what, file ? "yes" : "no", file ? 0 : errno);
	if (file)
		(void)fclose(file);
}

// Version 1: the store's way first, the new .xex renamed over the old; if that fails, the old renamed away first.
// Whether the new one took the old one's place.
static bool replace(const char *own)
{
	const long new_bytes = file_size(NEW);
	note("probe_replace: version 1 running from %s, %ld bytes, with %s of %ld bytes", own, file_size(own), NEW,
		new_bytes);
	if (new_bytes < 0)
		return false;
	opens(own, "rb", "to read");
	opens(own, "r+b", "to write");
	if (rename(NEW, own) == 0) {
		note("probe_replace: rename(%s, %s) over the running .xex: done", NEW, own);
	} else {
		note("probe_replace: rename(%s, %s) over the running .xex: errno %d", NEW, own, errno);
		const int away = rename(own, OLD) == 0 ? 0 : errno;
		const int in = away == 0 ? (rename(NEW, own) == 0 ? 0 : errno) : -1;
		const int removed = away == 0 ? (remove(OLD) == 0 ? 0 : errno) : -1;
		note("probe_replace: the running .xex renamed to %s: errno %d; %s renamed in: %d; %s removed: %d", OLD, away,
			NEW, in, OLD, removed);
	}
	const bool replaced = file_size(own) == new_bytes && file_size(NEW) < 0;
	note("probe_replace: %s is now %ld bytes, %s %s", own, file_size(own), NEW,
		file_size(NEW) < 0 ? "is gone" : "is still there");
	return replaced;
}

int main(void)
{
	const char *const image = ExLoadedImageName ? ExLoadedImageName : "";
	const char *const slash = strrchr(image, '\\');
	const char *const own = slash ? slash + 1 : image;
	const int version = running_version();
	xsdk_log_write("probe_replace: %s, version %d by its name", image, version);

	if (version == 2) {
		FILE *const kept = fopen(NOTES, "r");
		char line[LINE_BYTES];
		while (kept && fgets(line, sizeof(line), kept))
			xsdk_log_write("%.*s", (int)strcspn(line, "\r\n"), line);
		if (kept)
			(void)fclose(kept);
		xsdk_log_write("probe_replace: version 2 started from %s; %s", own,
			kept ? "version 1 replaced its own .xex and launched it" : "no notes from version 1");
		(void)remove(NOTES);
		xsdk_log_write("probe_replace: done");
		return 0;
	}

	notes = fopen(NOTES, "w");
	const bool replaced = replace(own);
	if (notes)
		(void)fclose(notes);
	// Only after a replacement, so a start without probe_replace.new can't launch itself over and over.
	if (replaced) {
		xsdk_log_write("probe_replace: launching %s", own);
		(void)xsdk_launch(own, NULL, 0);
		xsdk_log_write("probe_replace: xsdk_launch(%s) failed: errno %d", own, errno);
	}
	xsdk_log_write("probe_replace: done");
	return 1;
}
