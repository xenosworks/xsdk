#include <libgen.h>
#include <stdbool.h>
#include <string.h>

// POSIX's basename and dirname: newlib's libgen.h declares them, and this target's libc.a has neither, so a port that
// calls them didn't link. xsdk's paths take / and \ alike (src/path.c), so both separate.

static bool separator(char c)
{
	return c == '/' || c == '\\';
}

// The length of path without its trailing separators, or 1 if it's nothing but separators.
static size_t trimmed_length(const char *path)
{
	size_t length = strlen(path);
	while (length > 1 && separator(path[length - 1]))
		length--;
	return length;
}

char *basename(char *path)
{
	static char dot[] = ".";
	if (!path || !*path)
		return dot;
	const size_t length = trimmed_length(path);
	path[length] = '\0';
	size_t start = length;
	while (start > 0 && !separator(path[start - 1]))
		start--;
	// Nothing but separators leaves one, which is the name.
	return start == length ? path + length - 1 : path + start;
}

char *dirname(char *path)
{
	static char dot[] = ".";
	if (!path || !*path)
		return dot;
	size_t length = trimmed_length(path);
	if (length == 1 && separator(path[0])) {
		path[1] = '\0';
		return path;
	}
	while (length > 0 && !separator(path[length - 1]))
		length--;
	if (length == 0)
		return dot;
	while (length > 1 && separator(path[length - 1]))
		length--;
	path[length] = '\0';
	return path;
}
