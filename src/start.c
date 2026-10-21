#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "lock_private.h"
#include "microcode_private.h"
#include "pthread_private.h"
#include "reent_private.h"
#include "system_private.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

int main(int argc, char **argv);
void xsdk_start(void);

enum {
	// Xenia gives its command line 1024 bytes (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_module.cc:194-198).
	COMMAND_LINE_BYTES = 1024,
	MAX_ARGUMENTS = 32,
};

// Bounds that src/xsdk.ld defines.
extern void (*__preinit_array_start[])(void);
extern void (*__preinit_array_end[])(void);
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);
extern void (*__fini_array_start[])(void);
extern void (*__fini_array_end[])(void);
extern const char __eh_frame_start[];

// libgcc's unwinder (unwind-dw2-fde.h); with no ELF loader to ask, the title registers its frames itself.
void __register_frame_info(const void *begin, void *object);

// Room for libgcc's struct object, which is seven words today; crtbegin.o would normally provide it.
static uint32_t eh_frame_object[16];

// __cxa_atexit names the image by this; crtbegin.o would normally provide it.
void *__dso_handle = &__dso_handle;

static char command_line[COMMAND_LINE_BYTES];
static char *arguments[MAX_ARGUMENTS + 1];

static void run_fini_array(void)
{
	for (size_t i = (size_t)(__fini_array_end - __fini_array_start); i > 0; i--)
		__fini_array_start[i - 1]();
}

// Splits line in place at spaces, a quoted argument keeping its spaces and losing its quotes; returns the count. The
// console's holds only the .xex's name, in quotes (docs/HARDWARE.md, "XAM's sockets and loader, milestone 56").
static int split_command_line(char *line, char *argv[MAX_ARGUMENTS + 1])
{
	int argc = 0;
	while (argc < MAX_ARGUMENTS) {
		while (*line == ' ')
			line++;
		if (!*line)
			break;
		const bool quoted = *line == '"';
		line += quoted;
		argv[argc++] = line;
		while (*line && *line != (quoted ? '"' : ' '))
			line++;
		if (*line)
			*line++ = '\0';
	}
	argv[argc] = NULL;
	return argc;
}

void xsdk_start(void)
{
	xsdk_lock_init();
	xsdk_microcode_init();
	xsdk_pthread_init();
	xsdk_reent_init();
	(void)xsdk_log_open();
	xsdk_system_set_time_zone();
	__register_frame_info(__eh_frame_start, eh_frame_object);

	for (void (**function)(void) = __preinit_array_start; function < __preinit_array_end; function++)
		(*function)();
	for (void (**function)(void) = __init_array_start; function < __init_array_end; function++)
		(*function)();
	// Registered first, so it runs after the static destructors the constructors registered.
	(void)atexit(run_fini_array);

	(void)snprintf(command_line, sizeof command_line, "%s", ExLoadedCommandLine ? ExLoadedCommandLine : "");
	int argc = split_command_line(command_line, arguments);
	// argv[0] empty is C's "no program name" (C11 5.1.2.2.1); desktop code reads its arguments from argv[1].
	if (argc == 0) {
		static char program_name[] = "";
		arguments[0] = program_name;
		arguments[1] = NULL;
		argc = 1;
	}
	exit(main(argc, arguments));
}
