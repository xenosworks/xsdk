#ifndef XSDK_UTIME_H
#define XSDK_UTIME_H

#include <sys/utime.h>

// newlib's <utime.h> leaves utime to each system's own; xsdk's sets the times through the kernel (src/path.c).

#ifdef __cplusplus
extern "C" {
#endif

int utime(const char *path, const struct utimbuf *times);

#ifdef __cplusplus
}
#endif

#endif
