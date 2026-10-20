#ifndef XSDK_SYS_STATVFS_H
#define XSDK_SYS_STATVFS_H

#include <sys/types.h>

// POSIX's file system sizes, from the kernel's size class (src/file.c); newlib has no header for them.

#ifdef __cplusplus
extern "C" {
#endif

struct statvfs {
	unsigned long f_bsize;
	unsigned long f_frsize;
	fsblkcnt_t f_blocks;
	fsblkcnt_t f_bfree;
	fsblkcnt_t f_bavail;
	fsfilcnt_t f_files;
	fsfilcnt_t f_ffree;
	fsfilcnt_t f_favail;
	unsigned long f_fsid;
	unsigned long f_flag;
	unsigned long f_namemax;
};

enum {
	ST_RDONLY = 1,
	ST_NOSUID = 2,
};

int statvfs(const char *path, struct statvfs *info);
int fstatvfs(int fd, struct statvfs *info);

#ifdef __cplusplus
}
#endif

#endif
