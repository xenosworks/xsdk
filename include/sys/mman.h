#ifndef XSDK_SYS_MMAN_H
#define XSDK_SYS_MMAN_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE     0
#define PROT_READ     1
#define PROT_WRITE    2
#define PROT_EXEC     4
#define MAP_SHARED    1
#define MAP_PRIVATE   2
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FAILED    ((void *)-1)
#define MS_ASYNC      1
#define MS_INVALIDATE 2
#define MS_SYNC       4

// Anonymous private mappings only, 4 KB pages; address is a hint and may be ignored. PROT_EXEC is unsupported.
void *mmap(void *address, size_t length, int protection, int flags, int descriptor, off_t offset);
int munmap(void *address, size_t length);
int mprotect(void *address, size_t length, int protection);
int msync(void *address, size_t length, int flags);
int mlock(const void *address, size_t length);
int munlock(const void *address, size_t length);

#ifdef __cplusplus
}
#endif
#endif
