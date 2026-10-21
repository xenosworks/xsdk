#include "file_private.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "path_private.h"
#include "socket_private.h"
#include "time_private.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"

// newlib's descriptor calls on Nt*File; descriptors 0-2 are the console, and stdout and stderr go to the log. Sockets
// take the descriptors after these (src/socket.c), and close, read, write, fstat and isatty hand them on.

enum {
	FILE_SLOTS = 32,
	FIRST_FILE_FD = 3,
	ZERO_CHUNK_BYTES = 4096,
};

// A slot whose handle is being opened.
static const HANDLE SLOT_CLAIMED = 0xFFFFFFFF;

typedef struct {
	HANDLE handle;
	uint64_t position;
	bool append;
	bool directory;
	// A directory's kernel path, which openat and unlinkat resolve against.
	char path[XSDK_PATH_BYTES];
} FileSlot;

_Static_assert(FIRST_FILE_FD + FILE_SLOTS == XSDK_FIRST_SOCKET_FD, "sockets take the descriptors after files");

static FileSlot files[FILE_SLOTS];

int xsdk_file_errno(NTSTATUS status)
{
	switch (status) {
	case STATUS_OBJECT_NAME_NOT_FOUND:
	case STATUS_OBJECT_PATH_NOT_FOUND:
	case STATUS_NO_SUCH_FILE:
	// A drive with nothing in it.
	case STATUS_NO_MEDIA_IN_DEVICE:
	case STATUS_DEVICE_REMOVED:
		return ENOENT;
	case STATUS_OBJECT_NAME_COLLISION:
		return EEXIST;
	case STATUS_OBJECT_NAME_INVALID:
	case STATUS_INVALID_PARAMETER:
		return EINVAL;
	case STATUS_ACCESS_DENIED:
		return EACCES;
	case STATUS_SHARING_VIOLATION:
		return EBUSY;
	case STATUS_NO_MEMORY:
		return ENOMEM;
	case STATUS_DISK_FULL:
		return ENOSPC;
	case STATUS_INVALID_HANDLE:
		return EBADF;
	case STATUS_FILE_IS_A_DIRECTORY:
		return EISDIR;
	case STATUS_NOT_A_DIRECTORY:
		return ENOTDIR;
	case STATUS_DIRECTORY_NOT_EMPTY:
		return ENOTEMPTY;
	default:
		return EIO;
	}
}

static int fail_with_status(const char *call, int fd, NTSTATUS status)
{
	xsdk_log_write("%s(fd %d) failed: 0x%08X", call, fd, (unsigned)status);
	errno = xsdk_file_errno(status);
	return -1;
}

static FileSlot *claim_slot(int *fd)
{
	for (int i = 0; i < FILE_SLOTS; i++) {
		HANDLE expected = 0;
		if (__atomic_compare_exchange_n(
				&files[i].handle, &expected, SLOT_CLAIMED, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
			*fd = FIRST_FILE_FD + i;
			return &files[i];
		}
	}
	return NULL;
}

static void release_slot(FileSlot *file)
{
	__atomic_store_n(&file->handle, 0, __ATOMIC_RELEASE);
}

static FileSlot *find_slot(int fd)
{
	if (fd < FIRST_FILE_FD || fd >= FIRST_FILE_FD + FILE_SLOTS)
		return NULL;
	FileSlot *file = &files[fd - FIRST_FILE_FD];
	const HANDLE handle = __atomic_load_n(&file->handle, __ATOMIC_ACQUIRE);
	return handle && handle != SLOT_CLAIMED ? file : NULL;
}

static bool is_console(int fd)
{
	return fd >= 0 && fd < FIRST_FILE_FD;
}

// The console's descriptors and sockets, which have no file, so no position or size.
static bool is_stream(int fd)
{
	return is_console(fd) || xsdk_socket_fd(fd);
}

// A file's slot; a stream has none, which is EINVAL, as POSIX allows.
static FileSlot *find_file(int fd)
{
	FileSlot *file = find_slot(fd);
	if (!file)
		errno = is_stream(fd) ? EINVAL : EBADF;
	return file;
}

static uint32_t access_from_flags(int flags)
{
	switch (flags & O_ACCMODE) {
	case O_WRONLY:
		return GENERIC_WRITE;
	case O_RDWR:
		return GENERIC_READ | GENERIC_WRITE;
	default:
		return GENERIC_READ;
	}
}

static uint32_t disposition_from_flags(int flags)
{
	if (flags & O_CREAT) {
		if (flags & O_EXCL)
			return FILE_CREATE;
		return flags & O_TRUNC ? FILE_OVERWRITE_IF : FILE_OPEN_IF;
	}
	return flags & O_TRUNC ? FILE_OVERWRITE : FILE_OPEN;
}

static NTSTATUS query_information(HANDLE handle, FILE_NETWORK_OPEN_INFORMATION *information)
{
	IO_STATUS_BLOCK io_status;
	return NtQueryInformationFile(
		handle, &io_status, information, sizeof(*information), FILE_NETWORK_OPEN_INFORMATION_CLASS);
}

static NTSTATUS query_size(HANDLE handle, uint64_t *size_bytes)
{
	FILE_NETWORK_OPEN_INFORMATION information;
	const NTSTATUS status = query_information(handle, &information);
	if (NT_SUCCESS(status))
		*size_bytes = information.end_of_file;
	return status;
}

static NTSTATUS set_end(HANDLE handle, uint64_t end_bytes)
{
	const FILE_END_OF_FILE_INFORMATION end = { .end_of_file = end_bytes };
	IO_STATUS_BLOCK io_status;
	return NtSetInformationFile(handle, &io_status, &end, sizeof(end), FILE_END_OF_FILE_INFORMATION_CLASS);
}

static NTSTATUS write_zeros(HANDLE handle, uint64_t from_bytes, uint64_t to_bytes)
{
	static const uint8_t zeros[ZERO_CHUNK_BYTES];
	NTSTATUS status = STATUS_SUCCESS;
	for (uint64_t position = from_bytes; NT_SUCCESS(status) && position < to_bytes;) {
		const uint32_t chunk_bytes =
			to_bytes - position < sizeof(zeros) ? (uint32_t)(to_bytes - position) : sizeof(zeros);
		IO_STATUS_BLOCK io_status;
		status = NtWriteFile(handle, 0, NULL, NULL, &io_status, zeros, chunk_bytes, &position);
		position += chunk_bytes;
	}
	return status;
}

NTSTATUS xsdk_file_set_size(HANDLE handle, uint64_t size_bytes)
{
	uint64_t old_size;
	NTSTATUS status = query_size(handle, &old_size);
	if (!NT_SUCCESS(status))
		return status;
	status = set_end(handle, size_bytes);
	if (!NT_SUCCESS(status) || size_bytes <= old_size)
		return status;
	// A file grown again reads back its old bytes, not zeros (results/2026-10-20-milestone-54/step2_xsdk_test.log);
	// set, then zeroed, it reads back zeros (step3_xsdk_test_ftruncate_zeros.log).
	status = write_zeros(handle, old_size, size_bytes);
	// A call that fails leaves the file as it was.
	if (!NT_SUCCESS(status))
		(void)set_end(handle, old_size);
	return status;
}

HANDLE xsdk_file_handle(int fd)
{
	const FileSlot *file = find_slot(fd);
	return file ? file->handle : 0;
}

int xsdk_file_directory_path(int fd, char path[XSDK_PATH_BYTES])
{
	const FileSlot *file = find_slot(fd);
	if (!file)
		return EBADF;
	if (!file->directory)
		return ENOTDIR;
	memcpy(path, file->path, XSDK_PATH_BYTES);
	return 0;
}

void xsdk_file_stat(const FILE_NETWORK_OPEN_INFORMATION *information, struct stat *info)
{
	memset(info, 0, sizeof(*info));
	// The kernel keeps attributes, not owners or permissions, so everyone may do anything.
	info->st_mode = information->attributes & FILE_ATTRIBUTE_DIRECTORY ? S_IFDIR | ACCESSPERMS : S_IFREG | DEFFILEMODE;
	info->st_nlink = 1;
	info->st_size = (off_t)information->end_of_file;
	info->st_atim = xsdk_time_from_nt(information->last_access_time);
	info->st_mtim = xsdk_time_from_nt(information->last_write_time);
	info->st_ctim = xsdk_time_from_nt(information->change_time);
}

int openat(int dir_fd, const char *path, int flags, ...)
{
	char resolved[XSDK_PATH_BYTES];
	const int error = xsdk_path_resolve(dir_fd, path, resolved);
	if (error) {
		errno = error;
		return -1;
	}

	int fd;
	FileSlot *file = claim_slot(&fd);
	if (!file) {
		errno = EMFILE;
		return -1;
	}

	ANSI_STRING name;
	OBJECT_ATTRIBUTES attributes;
	xsdk_path_attributes(resolved, &name, &attributes);
	const bool directory = (flags & O_DIRECTORY) != 0;
	IO_STATUS_BLOCK io_status;
	HANDLE handle;
	// POSIX lets a file be open more than once; with no share access a second open fails, 0xC0000043.
	const NTSTATUS status = NtCreateFile(&handle, access_from_flags(flags) | SYNCHRONIZE, &attributes, &io_status, NULL,
		FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ | FILE_SHARE_WRITE, disposition_from_flags(flags),
		FILE_SYNCHRONOUS_IO_NONALERT | (directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE));
	if (!NT_SUCCESS(status)) {
		release_slot(file);
		xsdk_log_write("open: NtCreateFile(%s) failed: 0x%08X", resolved, (unsigned)status);
		errno = xsdk_file_errno(status);
		return -1;
	}
	file->position = 0;
	file->append = (flags & O_APPEND) != 0;
	file->directory = directory;
	memcpy(file->path, resolved, sizeof(resolved));
	__atomic_store_n(&file->handle, handle, __ATOMIC_RELEASE);
	return fd;
}

int open(const char *path, int flags, ...)
{
	return openat(AT_FDCWD, path, flags);
}

int close(int fd)
{
	if (is_console(fd))
		return 0;
	if (xsdk_socket_fd(fd))
		return xsdk_socket_close(fd);
	FileSlot *file = find_slot(fd);
	if (!file) {
		errno = EBADF;
		return -1;
	}
	const NTSTATUS status = NtClose(file->handle);
	release_slot(file);
	return NT_SUCCESS(status) ? 0 : fail_with_status("close", fd, status);
}

static ssize_t read_at(
	const char *call, int fd, const FileSlot *file, void *buffer, size_t length_bytes, uint64_t position)
{
	IO_STATUS_BLOCK io_status;
	const NTSTATUS status =
		NtReadFile(file->handle, 0, NULL, NULL, &io_status, buffer, (uint32_t)length_bytes, &position);
	if (status == STATUS_END_OF_FILE)
		return 0;
	if (!NT_SUCCESS(status))
		return fail_with_status(call, fd, status);
	return (ssize_t)io_status.information;
}

ssize_t read(int fd, void *buffer, size_t length_bytes)
{
	if (xsdk_socket_fd(fd))
		return xsdk_socket_read(fd, buffer, length_bytes);
	FileSlot *file = find_slot(fd);
	if (!file) {
		errno = EBADF;
		return -1;
	}
	const ssize_t read_bytes = read_at("read", fd, file, buffer, length_bytes, file->position);
	if (read_bytes > 0)
		file->position += (uint64_t)read_bytes;
	return read_bytes;
}

ssize_t pread(int fd, void *buffer, size_t length_bytes, off_t offset)
{
	const FileSlot *file = find_slot(fd);
	if (!file) {
		errno = is_stream(fd) ? ESPIPE : EBADF;
		return -1;
	}
	if (offset < 0) {
		errno = EINVAL;
		return -1;
	}
	return read_at("pread", fd, file, buffer, length_bytes, (uint64_t)offset);
}

static ssize_t write_at(
	const char *call, int fd, const FileSlot *file, const void *buffer, size_t length_bytes, uint64_t position)
{
	IO_STATUS_BLOCK io_status;
	const NTSTATUS status =
		NtWriteFile(file->handle, 0, NULL, NULL, &io_status, buffer, (uint32_t)length_bytes, &position);
	if (!NT_SUCCESS(status))
		return fail_with_status(call, fd, status);
	return (ssize_t)io_status.information;
}

ssize_t write(int fd, const void *buffer, size_t length_bytes)
{
	if (fd == STDOUT_FILENO || fd == STDERR_FILENO) {
		xsdk_log_write_bytes(buffer, length_bytes);
		return (ssize_t)length_bytes;
	}
	if (xsdk_socket_fd(fd))
		return xsdk_socket_write(fd, buffer, length_bytes);
	FileSlot *file = find_slot(fd);
	if (!file) {
		errno = EBADF;
		return -1;
	}
	if (file->append) {
		const NTSTATUS status = query_size(file->handle, &file->position);
		if (!NT_SUCCESS(status))
			return fail_with_status("write", fd, status);
	}
	const ssize_t written_bytes = write_at("write", fd, file, buffer, length_bytes, file->position);
	if (written_bytes > 0)
		file->position += (uint64_t)written_bytes;
	return written_bytes;
}

// At the offset given, even with O_APPEND, as POSIX has it.
ssize_t pwrite(int fd, const void *buffer, size_t length_bytes, off_t offset)
{
	const FileSlot *file = find_slot(fd);
	if (!file) {
		errno = is_stream(fd) ? ESPIPE : EBADF;
		return -1;
	}
	if (offset < 0) {
		errno = EINVAL;
		return -1;
	}
	return write_at("pwrite", fd, file, buffer, length_bytes, (uint64_t)offset);
}

int ftruncate(int fd, off_t length_bytes)
{
	const FileSlot *file = find_file(fd);
	if (!file)
		return -1;
	if (length_bytes < 0) {
		errno = EINVAL;
		return -1;
	}
	const NTSTATUS status = xsdk_file_set_size(file->handle, (uint64_t)length_bytes);
	return NT_SUCCESS(status) ? 0 : fail_with_status("ftruncate", fd, status);
}

int fsync(int fd)
{
	const FileSlot *file = find_file(fd);
	if (!file)
		return -1;
	IO_STATUS_BLOCK io_status;
	const NTSTATUS status = NtFlushBuffersFile(file->handle, &io_status);
	return NT_SUCCESS(status) ? 0 : fail_with_status("fsync", fd, status);
}

// The kernel has one flush, for data and attributes both.
int fdatasync(int fd)
{
	return fsync(fd);
}

off_t lseek(int fd, off_t offset, int whence)
{
	FileSlot *file = find_slot(fd);
	if (!file) {
		errno = is_stream(fd) ? ESPIPE : EBADF;
		return -1;
	}
	uint64_t base = 0;
	if (whence == SEEK_CUR) {
		base = file->position;
	} else if (whence == SEEK_END) {
		const NTSTATUS status = query_size(file->handle, &base);
		if (!NT_SUCCESS(status))
			return fail_with_status("lseek", fd, status);
	} else if (whence != SEEK_SET) {
		errno = EINVAL;
		return -1;
	}
	const int64_t position = (int64_t)base + offset;
	if (position < 0) {
		errno = EINVAL;
		return -1;
	}
	file->position = (uint64_t)position;
	return (off_t)position;
}

NTSTATUS xsdk_file_statvfs(HANDLE handle, struct statvfs *info)
{
	FILE_FS_SIZE_INFORMATION size;
	IO_STATUS_BLOCK io_status;
	const NTSTATUS status =
		NtQueryVolumeInformationFile(handle, &io_status, &size, sizeof(size), FILE_FS_SIZE_INFORMATION_CLASS);
	if (!NT_SUCCESS(status))
		return status;
	// The size class has no counts of files or length of names, so those stay 0.
	memset(info, 0, sizeof(*info));
	info->f_bsize = size.sectors_per_allocation_unit * size.bytes_per_sector;
	info->f_frsize = info->f_bsize;
	info->f_blocks = size.total_allocation_units;
	info->f_bfree = size.available_allocation_units;
	info->f_bavail = size.available_allocation_units;
	return STATUS_SUCCESS;
}

int fstatvfs(int fd, struct statvfs *info)
{
	const FileSlot *file = find_file(fd);
	if (!file)
		return -1;
	const NTSTATUS status = xsdk_file_statvfs(file->handle, info);
	return NT_SUCCESS(status) ? 0 : fail_with_status("fstatvfs", fd, status);
}

int fstat(int fd, struct stat *info)
{
	if (is_console(fd)) {
		memset(info, 0, sizeof(*info));
		info->st_mode = S_IFCHR;
		return 0;
	}
	if (xsdk_socket_fd(fd)) {
		memset(info, 0, sizeof(*info));
		info->st_mode = S_IFSOCK;
		return 0;
	}
	FileSlot *file = find_slot(fd);
	if (!file) {
		errno = EBADF;
		return -1;
	}
	FILE_NETWORK_OPEN_INFORMATION information;
	const NTSTATUS status = query_information(file->handle, &information);
	if (!NT_SUCCESS(status))
		return fail_with_status("fstat", fd, status);
	xsdk_file_stat(&information, info);
	return 0;
}

// Permissions can't change (xsdk_file_stat), so only asking for the ones a file already has succeeds.
int fchmod(int fd, mode_t mode)
{
	struct stat info;
	if (fstat(fd, &info) != 0)
		return -1;
	if ((mode & ALLPERMS) != (info.st_mode & ALLPERMS)) {
		errno = ENOTSUP;
		return -1;
	}
	return 0;
}

int isatty(int fd)
{
	if (is_console(fd))
		return 1;
	errno = find_slot(fd) || xsdk_socket_fd(fd) ? ENOTTY : EBADF;
	return 0;
}
