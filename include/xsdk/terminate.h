#ifndef XSDK_TERMINATE_H
#define XSDK_TERMINATE_H

#ifdef __cplusplus
extern "C" {
#endif

// Xbox Home, or anything else outside the title, ends it without exit: no atexit handler or destructor runs, and what
// stdio holds unwritten is lost (docs/GUIDE.md, "A title's life").

// Has routine run as the title is ended from outside; the first call registers it with the kernel, a later one
// replaces it, and NULL leaves nothing to run. The console runs it on a thread of its own, the title's threads all
// frozen where they are, its file handles closed and game: gone. So it takes no lock the title's threads may hold,
// uses no stdio, malloc or other newlib state, and reaches files only through raw kernel calls on device paths, such
// as NtCreateFile of "\Device\Harddisk0\Partition1\..."; it was given 2 s, so it takes far less. xsdk takes the
// registration out once it returns, as one left in place froze the console as the next title started, and at exit,
// where the routine doesn't run (docs/HARDWARE.md, "Drives, time zones and Xbox Home, milestone 54").
void xsdk_on_terminate(void (*routine)(void));

#ifdef __cplusplus
}
#endif

#endif
