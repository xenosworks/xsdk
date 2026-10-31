#ifndef XSDK_SYS_UTSNAME_H
#define XSDK_SYS_UTSNAME_H

// POSIX's names of the system (src/system.c); newlib has no header for them.

#ifdef __cplusplus
extern "C" {
#endif

enum {
	// Linux's, which ports may count on.
	XSDK_UTSNAME_BYTES = 65,
};

// sysname "Xbox 360"; release the kernel's version, XboxKrnlVersion's four numbers; machine "ppc", as the title's code
// is 32-bit; nodename and version empty.
struct utsname {
	char sysname[XSDK_UTSNAME_BYTES];
	char nodename[XSDK_UTSNAME_BYTES];
	char release[XSDK_UTSNAME_BYTES];
	char version[XSDK_UTSNAME_BYTES];
	char machine[XSDK_UTSNAME_BYTES];
};

int uname(struct utsname *name);

#ifdef __cplusplus
}
#endif

#endif
