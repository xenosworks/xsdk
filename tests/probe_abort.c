#include <stdlib.h>

#include "xsdk/log.h"

// Whether kill's log names abort's callers: abort from two nested functions, which the log's addresses must name
// through addr2line on probe_abort.elf.

__attribute__((noinline)) static void inner(int depth)
{
	xsdk_log_write("abort probe: aborting %d deep", depth);
	abort();
}

__attribute__((noinline)) static void outer(int depth)
{
	inner(depth + 1);
	xsdk_log_write("abort probe: not reached");
}

int main(void)
{
	outer(1);
	return 0;
}
