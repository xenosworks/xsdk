#ifndef XSDK_FILE_PRIVATE_H
#define XSDK_FILE_PRIVATE_H

#include <sys/stat.h>
#include <sys/statvfs.h>

#include "xsdk/kernel.h"

enum {
	// NT's MAX_PATH, counting the terminating NUL.
	XSDK_PATH_BYTES = 260,
};

// The errno closest to a kernel status.
int xsdk_file_errno(NTSTATUS status);
// The kernel handle behind an open descriptor, or 0.
HANDLE xsdk_file_handle(int fd);
// Copies the kernel path of a descriptor opened with O_DIRECTORY; returns 0 or an errno.
int xsdk_file_directory_path(int fd, char path[XSDK_PATH_BYTES]);
// resolved is the file's kernel path, which stands for its identity in st_dev and st_ino.
void xsdk_file_stat(const FILE_NETWORK_OPEN_INFORMATION *information, const char *resolved, struct stat *info);
// The attributes to set for chmod's mode: the owner's write bit stands for the read-only attribute, as in stat.
uint32_t xsdk_file_mode_attributes(uint32_t attributes, mode_t mode);
// 0 if attributes, read back after a chmod, hold mode's write bit; otherwise -1 with errno EPERM.
int xsdk_file_mode_kept(uint32_t attributes, mode_t mode);
NTSTATUS xsdk_file_set_basic(HANDLE handle, const FILE_BASIC_INFORMATION *information);
// The flushes the kernel has taken from fsync and sync, for a test to count.
uint32_t xsdk_file_flushes(void);
// Sets a file's size, writing zeros over the bytes it adds.
NTSTATUS xsdk_file_set_size(HANDLE handle, uint64_t size_bytes);
// Fills info with the sizes of the volume handle is on.
NTSTATUS xsdk_file_statvfs(HANDLE handle, struct statvfs *info);

#endif
