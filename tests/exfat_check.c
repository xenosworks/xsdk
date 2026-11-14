#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/splash.h"
#include "xsdk/thread.h"

// Milestone 77 step 3 (docs/scope/77-exfat.md): the exFAT module (plugins/exfat/exfat_module.c) checked through xsdk's
// file calls on the first USB stick, usb0:, which holds an exFAT volume made on the PC; exfat_load.xex has loaded the
// module. Each check can fail: a stick the kernel's own drivers mount fails the file system's name and the run stops
// there; a request the module gets wrong fails its check. What xsdk's calls can't reach goes through the kernel's
// directly: share access, offsets the kernel keeps, a directory entry's fields, and everything past 2 GB, as off_t is
// 32 bits in this toolchain. Leaves exfat_check\verify.bin and exfat_check\big.bin on the stick for the PC's byte
// checks (results/2026-11-14-milestone-77-step-3/README.md); everything else it made is removed.

enum {
	KERNEL_BUILD = 17559,
	SPLASH_MS = 1000,
	MOUNT_WAIT_MS = 5000,
	MOUNT_POLL_MS = 100,
	CHUNK_BYTES = 1 << 20,
	PIECE_BYTES = 65536,
	APPEND_BYTES = 100 << 10,
	// 4 GiB + 1 MiB, written a chunk at a time.
	BIG_CHUNKS = 4097,
	VERIFY_CHUNK = BIG_CHUNKS,
	FOUR_MIB_CHUNKS = 4,
	NAMES_MAX = 64,
	NAME_BYTES = 256,
	DEPTH_MAX = 4,
	PATH_BYTES = 512,
	// "\Device\Mass0\" and the module's longest path under the volume, 519 bytes, with the terminator.
	LONG_PATH_BYTES = 560,
	LONG_NAME_BYTES = NAME_BYTES + 1,
	// Directories of 120 characters, four deep under exfat_check: 11 + 4 * 121 = 495 bytes under the volume.
	DEEP_LEVELS = 4,
	DEEP_NAME_CHARS = 120,
	MTIME_WINDOW_S = 60,
	FILE_FS_ATTRIBUTE_INFORMATION_CLASS = 5,
	FILE_POSITION_INFORMATION_CLASS = 14,
};
static const uint64_t FOUR_GIB = (uint64_t)4096 << 20;
static const uint64_t BIG_BYTES = (uint64_t)BIG_CHUNKS << 20;
static const uint64_t BIG_CUT_BYTES = ((uint64_t)4096 << 20) + 4096;
static const char ROOT[] = "usb0:/";
static const char FOLDER[] = "usb0:/exfat_check";
static const char DEVICE_ROOT[] = "\\Device\\Mass0\\";
static const char PATTERN[] = "usb0:/exfat_check/pattern.bin";
static const char DEVICE_PATTERN[] = "\\Device\\Mass0\\exfat_check\\pattern.bin";
static const char BIG[] = "usb0:/exfat_check/big.bin";
static const char DEVICE_BIG[] = "\\Device\\Mass0\\exfat_check\\big.bin";

static unsigned passed_count;
static unsigned failed_count;
// chunk keeps chunk 0, pattern.bin's bytes, for the whole run; scratch is refilled; back takes what is read.
static _Alignas(4096) uint8_t chunk[CHUNK_BYTES];
static _Alignas(4096) uint8_t scratch[CHUNK_BYTES];
static _Alignas(4096) uint8_t back[CHUNK_BYTES + 16];
static char names[DEPTH_MAX][NAMES_MAX][NAME_BYTES];
static uint8_t types[DEPTH_MAX][NAMES_MAX];

static void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
	passed_count += passed;
	failed_count += !passed;
}

static void check_ok(const char *name, int result)
{
	if (result != 0)
		xsdk_log_write("  %s: result %d, errno %d", name, result, errno);
	check(name, result == 0);
}

static void check_errno(const char *name, int result, int expected)
{
	const bool passed = result < 0 && errno == expected;
	if (!passed)
		xsdk_log_write("  %s: result %d, errno %d, expected errno %d", name, result, result < 0 ? errno : 0, expected);
	check(name, passed);
}

// Word j of chunk k, big-endian in the file: the PC's check regenerates it (results/.../check_stick.py).
static uint32_t chunk_word(uint32_t k, uint32_t j)
{
	return (j * 0x01010101u) ^ (k * 0x9E3779B1u + 0x7F4A7C15u);
}

static void fill_chunk(uint8_t *buffer, uint32_t k)
{
	uint32_t *const words = (uint32_t *)buffer;
	for (uint32_t j = 0; j < CHUNK_BYTES / 4; j++)
		words[j] = chunk_word(k, j);
}

static bool all_zero(const uint8_t *bytes, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (bytes[i])
			return false;
	}
	return true;
}

static uint64_t now_ms(void)
{
	struct timespec time;
	(void)clock_gettime(CLOCK_MONOTONIC, &time);
	return (uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000;
}

static ANSI_STRING ansi(const char *text)
{
	const ANSI_STRING string = { (uint16_t)strlen(text), (uint16_t)(strlen(text) + 1), text };
	return string;
}

static NTSTATUS open_nt(
	const char *path, uint32_t access, uint32_t share, uint32_t disposition, uint32_t options, HANDLE *handle)
{
	const ANSI_STRING name = ansi(path);
	const OBJECT_ATTRIBUTES attributes = { 0, &name, 0 };
	IO_STATUS_BLOCK io;
	*handle = 0;
	return NtCreateFile(handle, access | SYNCHRONIZE, &attributes, &io, NULL, FILE_ATTRIBUTE_NORMAL, share, disposition,
		options | FILE_SYNCHRONOUS_IO_NONALERT);
}

static bool read_nt(HANDLE handle, void *buffer, uint32_t bytes, uint64_t offset)
{
	IO_STATUS_BLOCK io = { 0, 0 };
	const NTSTATUS status = NtReadFile(handle, 0, NULL, NULL, &io, buffer, bytes, &offset);
	if (!NT_SUCCESS(status) || io.information != bytes)
		xsdk_log_write("  NtReadFile of %u at %llu: 0x%08X, %u bytes", bytes, (unsigned long long)offset,
			(unsigned)status, io.information);
	return NT_SUCCESS(status) && io.information == bytes;
}

static NTSTATUS size_nt(const char *device_path, uint64_t *size)
{
	const ANSI_STRING name = ansi(device_path);
	const OBJECT_ATTRIBUTES attributes = { 0, &name, 0 };
	FILE_NETWORK_OPEN_INFORMATION information;
	const NTSTATUS status = NtQueryFullAttributesFile(&attributes, &information);
	*size = NT_SUCCESS(status) ? information.end_of_file : 0;
	return status;
}

static bool write_small(const char *path, const void *bytes, size_t count)
{
	const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
		return false;
	const bool written = write(fd, bytes, count) == (ssize_t)count;
	return close(fd) == 0 && written;
}

// The file's bytes against expected: the same count and the same bytes.
static bool holds(const char *path, const void *expected, size_t count)
{
	const int fd = open(path, O_RDONLY);
	if (fd < 0)
		return false;
	const ssize_t got = read(fd, back, count + 1);
	(void)close(fd);
	return got == (ssize_t)count && memcmp(back, expected, count) == 0;
}

static void open_fails(const char *name, const char *path, int flags, int expected)
{
	const int fd = open(path, flags);
	if (fd >= 0)
		(void)close(fd);
	check_errno(name, fd, expected);
}

// The directory's entries, sorted; -1 when it can't be listed.
static int list_sorted(const char *path, unsigned depth)
{
	DIR *const dir = opendir(path);
	if (!dir)
		return -1;
	int count = 0;
	const struct dirent *entry;
	while (count < NAMES_MAX && (entry = readdir(dir))) {
		snprintf(names[depth][count], NAME_BYTES, "%s", entry->d_name);
		types[depth][count] = entry->d_type;
		count++;
	}
	(void)closedir(dir);
	// The types follow their names: sort both through an index.
	for (int i = 1; i < count; i++) {
		char name[NAME_BYTES];
		memcpy(name, names[depth][i], NAME_BYTES);
		const uint8_t type = types[depth][i];
		int j = i;
		while (j > 0 && strcmp(names[depth][j - 1], name) > 0) {
			memcpy(names[depth][j], names[depth][j - 1], NAME_BYTES);
			types[depth][j] = types[depth][j - 1];
			j--;
		}
		memcpy(names[depth][j], name, NAME_BYTES);
		types[depth][j] = type;
	}
	return count;
}

static bool listed(const char *path, unsigned depth, const char *name)
{
	const int count = list_sorted(path, depth);
	for (int i = 0; i < count; i++) {
		if (strcmp(names[depth][i], name) == 0)
			return true;
	}
	return false;
}

// Removes what is under path; the caller removes path.
static void remove_tree(const char *path, unsigned depth)
{
	const int count = list_sorted(path, depth);
	for (int i = 0; i < count; i++) {
		char child[PATH_BYTES];
		snprintf(child, sizeof child, "%s/%s", path, names[depth][i]);
		if (types[depth][i] == DT_DIR) {
			if (depth + 1 < DEPTH_MAX)
				remove_tree(child, depth + 1);
			(void)rmdir(child);
		} else {
			(void)unlink(child);
		}
	}
}

static void summary(void)
{
	xsdk_log_write("tests: %u passed, %u failed", passed_count, failed_count);
}

// The volume mounts within MOUNT_WAIT_MS, and the file system is the module's, not the kernel's.
static bool mounted_exfat(void)
{
	struct statvfs space;
	const uint64_t start = now_ms();
	int result;
	while ((result = statvfs(ROOT, &space)) != 0 && now_ms() - start < MOUNT_WAIT_MS)
		(void)xsdk_thread_sleep(MOUNT_POLL_MS);
	check("usb0: answers statvfs within 5 s", result == 0);
	if (result != 0)
		return false;
	xsdk_log_write("usb0: %lu clusters of %lu bytes, %lu free", (unsigned long)space.f_blocks,
		(unsigned long)space.f_frsize, (unsigned long)space.f_bfree);
	HANDLE volume;
	NTSTATUS status =
		open_nt(DEVICE_ROOT, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, FILE_DIRECTORY_FILE, &volume);
	// FILE_FS_ATTRIBUTE_INFORMATION: attributes, the longest component, the name's length, the name.
	_Alignas(8) uint8_t attribute[64];
	memset(attribute, 0, sizeof attribute);
	IO_STATUS_BLOCK io = { 0, 0 };
	if (NT_SUCCESS(status)) {
		status =
			NtQueryVolumeInformationFile(volume, &io, attribute, sizeof attribute, FILE_FS_ATTRIBUTE_INFORMATION_CLASS);
		(void)NtClose(volume);
	}
	uint32_t name_length;
	memcpy(&name_length, attribute + 8, 4);
	const bool exfat = NT_SUCCESS(status) && name_length == 5 && memcmp(attribute + 12, "exFAT", 5) == 0;
	xsdk_log_write("file system: status 0x%08X, %u bytes, name \"%.*s\"", (unsigned)status, io.information,
		(int)(name_length < 32 ? name_length : 32), (const char *)attribute + 12);
	check("the volume's file system is exFAT", exfat);
	return exfat;
}

static void check_pattern_file(void)
{
	struct stat info;
	fill_chunk(chunk, 0);
	int fd = open(PATTERN, O_WRONLY | O_CREAT | O_EXCL, 0666);
	check("create pattern.bin", fd >= 0);
	bool written = fd >= 0;
	for (uint32_t offset = 0; written && offset < CHUNK_BYTES; offset += PIECE_BYTES)
		written = write(fd, chunk + offset, PIECE_BYTES) == PIECE_BYTES;
	check("write 1 MiB in 64 KB pieces", written);
	check_ok("fsync pattern.bin", fsync(fd));
	check_ok("close pattern.bin", close(fd));
	const time_t now = time(NULL);
	const bool found = stat(PATTERN, &info) == 0;
	check("stat pattern.bin: a 1 MiB regular file", found && S_ISREG(info.st_mode) && info.st_size == CHUNK_BYTES);
	xsdk_log_write("  mtime %lld, now %lld", (long long)info.st_mtime, (long long)now);
	check("mtime within a minute of now",
		found && (info.st_mtime > now ? info.st_mtime - now : now - info.st_mtime) <= MTIME_WINDOW_S);

	fd = open(PATTERN, O_RDONLY);
	check("open pattern.bin for reading", fd >= 0);
	memset(back, 0, CHUNK_BYTES);
	bool read_back = fd >= 0;
	for (uint32_t offset = 0; read_back && offset < CHUNK_BYTES; offset += PIECE_BYTES)
		read_back = read(fd, back + offset, PIECE_BYTES) == PIECE_BYTES;
	check("read back 1 MiB in 64 KB pieces, identical", read_back && memcmp(chunk, back, CHUNK_BYTES) == 0);
	// Into an odd address: the module moves it through its own buffer.
	check("pread 13 at 777 into an odd address",
		pread(fd, back + 1, 13, 777) == 13 && memcmp(back + 1, chunk + 777, 13) == 0);
	check("pread 2 across 4096", pread(fd, back, 2, 4095) == 2 && memcmp(back, chunk + 4095, 2) == 0);
	check("pread 2 across 65536", pread(fd, back, 2, 65535) == 2 && memcmp(back, chunk + 65535, 2) == 0);
	check("pread over the end gives the rest",
		pread(fd, back, APPEND_BYTES, CHUNK_BYTES - 1000) == 1000 &&
			memcmp(back, chunk + CHUNK_BYTES - 1000, 1000) == 0);
	check("pread at the end gives 0", pread(fd, back, 16, CHUNK_BYTES) == 0);
	check("pread past the end gives 0", pread(fd, back, 16, CHUNK_BYTES + 5000) == 0);
	check("lseek SET 1000", lseek(fd, 1000, SEEK_SET) == 1000);
	check("read 10 at 1000", read(fd, back, 10) == 10 && memcmp(back, chunk + 1000, 10) == 0);
	check("lseek CUR -5 is 1005", lseek(fd, -5, SEEK_CUR) == 1005);
	check("lseek END is 1 MiB", lseek(fd, 0, SEEK_END) == CHUNK_BYTES);
	check("read at the end gives 0", read(fd, back, 16) == 0);
	check("lseek END -1 and read the last byte",
		lseek(fd, -1, SEEK_END) == CHUNK_BYTES - 1 && read(fd, back, 16) == 1 && back[0] == chunk[CHUNK_BYTES - 1]);
	check_ok("close pattern.bin", close(fd));

	fill_chunk(scratch, 1);
	fd = open(PATTERN, O_WRONLY | O_APPEND);
	check("append 100 KB", fd >= 0 && write(fd, scratch, APPEND_BYTES) == APPEND_BYTES);
	(void)close(fd);
	check("size is 1 MiB + 100 KB", stat(PATTERN, &info) == 0 && info.st_size == CHUNK_BYTES + APPEND_BYTES);
	fd = open(PATTERN, O_RDONLY);
	check("the appended bytes read back",
		fd >= 0 && pread(fd, back, APPEND_BYTES, CHUNK_BYTES) == APPEND_BYTES &&
			memcmp(back, scratch, APPEND_BYTES) == 0);
	(void)close(fd);

	fd = open(PATTERN, O_RDWR);
	check("open pattern.bin read-write", fd >= 0);
	check_ok("ftruncate to 300000", ftruncate(fd, 300000));
	check("size is 300000", fstat(fd, &info) == 0 && info.st_size == 300000);
	check_ok("ftruncate up to 400000", ftruncate(fd, 400000));
	memset(back, 0xAA, 100000);
	check("the grown bytes read as zeros", pread(fd, back, 100000, 300000) == 100000 && all_zero(back, 100000));
	check("the bytes before stay", pread(fd, back, 1000, 299000) == 1000 && memcmp(back, chunk + 299000, 1000) == 0);
	check("lseek to 405000 and write 10 bytes", lseek(fd, 405000, SEEK_SET) == 405000 && write(fd, scratch, 10) == 10);
	check("the gap reads as zeros and the 10 bytes follow",
		pread(fd, back, 5010, 400000) == 5010 && all_zero(back, 5000) && memcmp(back + 5000, scratch, 10) == 0);
	check_ok("close pattern.bin", close(fd));
	check_ok("truncate(path) to 123456", truncate(PATTERN, 123456));
	check("size is 123456", stat(PATTERN, &info) == 0 && info.st_size == 123456);
}

static void check_names(void)
{
	struct stat info;
	check("a.txt written", write_small("usb0:/exfat_check/a.txt", "AAAA", 4));
	check_ok("rename a.txt to b.txt", rename("usb0:/exfat_check/a.txt", "usb0:/exfat_check/b.txt"));
	check_errno("stat a.txt after the rename: ENOENT", stat("usb0:/exfat_check/a.txt", &info), ENOENT);
	check("b.txt holds a.txt's bytes", holds("usb0:/exfat_check/b.txt", "AAAA", 4));
	check_ok("mkdir sub", mkdir("usb0:/exfat_check/sub", 0777));
	check_ok("rename b.txt into sub", rename("usb0:/exfat_check/b.txt", "usb0:/exfat_check/sub/b.txt"));
	check("sub/b.txt is there", stat("usb0:/exfat_check/sub/b.txt", &info) == 0 && info.st_size == 4);
	check_ok("rename sub to sub2", rename("usb0:/exfat_check/sub", "usb0:/exfat_check/sub2"));
	check("sub2/b.txt is there after the directory's rename", stat("usb0:/exfat_check/sub2/b.txt", &info) == 0);
	check_errno("stat sub/b.txt: ENOENT", stat("usb0:/exfat_check/sub/b.txt", &info), ENOENT);
	check_ok("rename b.txt to B.TXT, the case alone",
		rename("usb0:/exfat_check/sub2/b.txt", "usb0:/exfat_check/sub2/B.TXT"));
	check("sub2 lists B.TXT alone",
		list_sorted("usb0:/exfat_check/sub2", 0) == 1 && strcmp(names[0][0], "B.TXT") == 0 && types[0][0] == DT_REG);
	check("c.txt and d.txt written",
		write_small("usb0:/exfat_check/c.txt", "CC", 2) && write_small("usb0:/exfat_check/d.txt", "DDDD", 4));
	check_ok("rename c.txt over d.txt", rename("usb0:/exfat_check/c.txt", "usb0:/exfat_check/d.txt"));
	check("d.txt holds c.txt's bytes", holds("usb0:/exfat_check/d.txt", "CC", 2));
	check_errno("stat c.txt: ENOENT", stat("usb0:/exfat_check/c.txt", &info), ENOENT);
	const int held = open("usb0:/exfat_check/d.txt", O_RDONLY);
	check("e.txt written", write_small("usb0:/exfat_check/e.txt", "E", 1));
	check_errno(
		"rename over an open file: EACCES", rename("usb0:/exfat_check/e.txt", "usb0:/exfat_check/d.txt"), EACCES);
	(void)close(held);
	check_ok("rename e.txt over d.txt once closed", rename("usb0:/exfat_check/e.txt", "usb0:/exfat_check/d.txt"));
	check("d.txt holds e.txt's byte", holds("usb0:/exfat_check/d.txt", "E", 1));

	check_ok("unlink d.txt", unlink("usb0:/exfat_check/d.txt"));
	check_errno("unlink d.txt again: ENOENT", unlink("usb0:/exfat_check/d.txt"), ENOENT);
	check_errno("rmdir sub2 with B.TXT inside: ENOTEMPTY", rmdir("usb0:/exfat_check/sub2"), ENOTEMPTY);
	check_errno("rmdir of a file: ENOTDIR", rmdir("usb0:/exfat_check/sub2/B.TXT"), ENOTDIR);
	check_ok("unlink sub2/B.TXT", unlink("usb0:/exfat_check/sub2/B.TXT"));
	check_ok("rmdir sub2", rmdir("usb0:/exfat_check/sub2"));
	check_errno("stat sub2: ENOENT", stat("usb0:/exfat_check/sub2", &info), ENOENT);

	check_ok("mkdir listing", mkdir("usb0:/exfat_check/listing", 0777));
	check("listing/a1.txt and b2.txt written",
		write_small("usb0:/exfat_check/listing/a1.txt", "1", 1) &&
			write_small("usb0:/exfat_check/listing/b2.txt", "22", 2));
	check_ok("mkdir listing/dir1", mkdir("usb0:/exfat_check/listing/dir1", 0777));
	const int count = list_sorted("usb0:/exfat_check/listing", 0);
	check("listing holds a1.txt, b2.txt and dir1, nothing else",
		count == 3 && strcmp(names[0][0], "a1.txt") == 0 && strcmp(names[0][1], "b2.txt") == 0 &&
			strcmp(names[0][2], "dir1") == 0);
	check("two files and a directory by d_type",
		count == 3 && types[0][0] == DT_REG && types[0][1] == DT_REG && types[0][2] == DT_DIR);
	DIR *const empty = opendir("usb0:/exfat_check/listing/dir1");
	errno = 0;
	check("an empty directory lists nothing, errno untouched", empty && readdir(empty) == NULL && errno == 0);
	if (empty)
		(void)closedir(empty);
	check("the root listing holds exfat_check", listed(ROOT, 0, "exfat_check"));

	// The kernel's own call: every field of the entry written, the pattern honoured, the end's statuses.
	HANDLE dir;
	NTSTATUS status = open_nt("\\Device\\Mass0\\exfat_check\\listing", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		FILE_OPEN, FILE_DIRECTORY_FILE, &dir);
	check("NtCreateFile of listing as a directory", NT_SUCCESS(status));
	_Alignas(8) uint8_t entry[512];
	memset(entry, 0xFF, sizeof entry);
	IO_STATUS_BLOCK io = { 0, 0 };
	const ANSI_STRING star = ansi("*");
	FILE_DIRECTORY_INFORMATION *const d = (FILE_DIRECTORY_INFORMATION *)entry;
	status = NtQueryDirectoryFile(dir, 0, NULL, NULL, &io, d, sizeof entry, &star, 1);
	check("NtQueryDirectoryFile: one entry, every field written",
		NT_SUCCESS(status) && d->next_entry_offset == 0 && d->file_index != UINT32_MAX &&
			d->creation_time != UINT64_MAX && d->last_access_time != UINT64_MAX && d->last_write_time != UINT64_MAX &&
			d->change_time != UINT64_MAX && d->end_of_file != UINT64_MAX && d->allocation_size != UINT64_MAX &&
			(d->attributes & ~0xB7u) == 0 && d->file_name_length > 0 && d->file_name_length < 64 &&
			io.information == 64 + d->file_name_length);
	xsdk_log_write("  entry: status 0x%08X index %u attributes 0x%X name %.*s", (unsigned)status, d->file_index,
		d->attributes, (int)(d->file_name_length < 64 ? d->file_name_length : 0), d->file_name);
	const ANSI_STRING b = ansi("b*");
	status = NtQueryDirectoryFile(dir, 0, NULL, NULL, &io, d, sizeof entry, &b, 1);
	check("the pattern b* finds b2.txt",
		NT_SUCCESS(status) && d->file_name_length == 6 && memcmp(d->file_name, "b2.txt", 6) == 0);
	status = NtQueryDirectoryFile(dir, 0, NULL, NULL, &io, d, sizeof entry, NULL, 0);
	check("then STATUS_NO_MORE_FILES", status == STATUS_NO_MORE_FILES);
	(void)NtClose(dir);
	status = open_nt("\\Device\\Mass0\\exfat_check\\listing\\dir1", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
		FILE_OPEN, FILE_DIRECTORY_FILE, &dir);
	NTSTATUS first = STATUS_SUCCESS;
	if (NT_SUCCESS(status)) {
		first = NtQueryDirectoryFile(dir, 0, NULL, NULL, &io, d, sizeof entry, &star, 1);
		(void)NtClose(dir);
	}
	check("an empty directory's first query: STATUS_NO_SUCH_FILE", NT_SUCCESS(status) && first == STATUS_NO_SUCH_FILE);

	open_fails("open a file with O_DIRECTORY: ENOTDIR", PATTERN, O_RDONLY | O_DIRECTORY, ENOTDIR);
	open_fails("open a directory as a file: EISDIR", "usb0:/exfat_check/listing", O_RDONLY, EISDIR);
	// STATUS_OBJECT_PATH_NOT_FOUND for both, as the kernel's FAT answers (plugins/exfat/kernel_seam.md, section 4).
	open_fails("open under a file: ENOENT", "usb0:/exfat_check/pattern.bin/x", O_RDONLY, ENOENT);
	open_fails("open under a missing directory: ENOENT", "usb0:/exfat_check/nowhere/x", O_RDONLY, ENOENT);

	static const char latin[] = "usb0:/exfat_check/caf\xe9.txt";
	check("a Latin-1 name written", write_small(latin, "cafe", 4));
	check("stat of the Latin-1 name", stat(latin, &info) == 0 && info.st_size == 4);
	check("the Latin-1 name lists with the same bytes", listed(FOLDER, 0, "caf\xe9.txt"));
	check_ok("unlink the Latin-1 name", unlink(latin));
}

// Marks the file or directory for deletion through the kernel and closes it, for paths xsdk's own calls can't take.
static NTSTATUS delete_nt(const char *device_path, uint32_t options)
{
	HANDLE handle;
	NTSTATUS status = open_nt(device_path, DELETE, 0, FILE_OPEN, options, &handle);
	if (!NT_SUCCESS(status))
		return status;
	const FILE_DISPOSITION_INFORMATION disposition = { 1 };
	IO_STATUS_BLOCK io;
	status = NtSetInformationFile(handle, &io, &disposition, sizeof disposition, FILE_DISPOSITION_INFORMATION_CLASS);
	(void)NtClose(handle);
	return status;
}

// The limits FATX's 42-character names never reached: exFAT's 255 characters a name and the module's 519 bytes of
// path under the volume (plugins/exfat/exfat_module.c, PATH_BYTES), through the kernel and through xsdk's own calls,
// whose resolved path holds 548 (src/file_private.h, XSDK_PATH_BYTES). The same names on game:, the title's folder on
// the FATX hdd, tell the file systems apart: FATX refuses them itself, so the room in xsdk gives it nothing.
static void check_long_names(void)
{
	struct stat info;
	uint64_t size;
	HANDLE handle;
	IO_STATUS_BLOCK io;
	char name[LONG_NAME_BYTES];
	char path[LONG_PATH_BYTES];
	char device_path[LONG_PATH_BYTES];

	// Through xsdk: a 255-character name on the stick works; on FATX, 42 works and 255 is FATX's own refusal,
	// STATUS_OBJECT_NAME_INVALID as EINVAL, not xsdk's ENAMETOOLONG.
	memset(name, 'x', 255);
	name[255] = 0;
	snprintf(path, sizeof path, "%s/%s", FOLDER, name);
	check("a 255-character name written through xsdk", write_small(path, "LONG", 4));
	check("it lists with its 255 characters", listed(FOLDER, 0, name));
	check("stat of the 255-character name", stat(path, &info) == 0 && info.st_size == 4);
	check_ok("unlink the 255-character name", unlink(path));
	snprintf(path, sizeof path, "game:/%s", name);
	const int fd = open(path, O_WRONLY | O_CREAT, 0666);
	if (fd >= 0) {
		(void)close(fd);
		(void)unlink(path);
	}
	check_errno("the 255-character name on FATX: EINVAL, its own refusal", fd, EINVAL);
	name[42] = 0;
	snprintf(path, sizeof path, "game:/%s", name);
	check("a 42-character name on FATX written", write_small(path, "FATX", 4));
	check_ok("unlink the 42-character name", unlink(path));

	// Through the kernel: 255 characters, exFAT's longest name, created, written, listed and deleted; 256 refused.
	memset(name, 'y', 255);
	name[255] = 0;
	snprintf(device_path, sizeof device_path, "%sexfat_check\\%s", DEVICE_ROOT, name);
	NTSTATUS status = open_nt(device_path, GENERIC_WRITE, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE, &handle);
	check("a 255-character name created through the kernel", NT_SUCCESS(status));
	bool written = false;
	if (NT_SUCCESS(status)) {
		status = NtWriteFile(handle, 0, NULL, NULL, &io, "LONG", 4, NULL);
		written = NT_SUCCESS(status) && io.information == 4;
		(void)NtClose(handle);
	}
	check("4 bytes written to it", written);
	check("it lists with its 255 characters", listed(FOLDER, 0, name));
	check("its size through the kernel: 4", NT_SUCCESS(size_nt(device_path, &size)) && size == 4);
	snprintf(path, sizeof path, "%s/%s", FOLDER, name);
	check("the same name through xsdk: stat finds it", stat(path, &info) == 0 && info.st_size == 4);
	check("delete the 255-character name through the kernel",
		NT_SUCCESS(delete_nt(device_path, FILE_NON_DIRECTORY_FILE)));
	check("it is gone", size_nt(device_path, &size) == STATUS_OBJECT_NAME_NOT_FOUND);
	name[255] = 'y';
	name[256] = 0;
	snprintf(device_path, sizeof device_path, "%sexfat_check\\%s", DEVICE_ROOT, name);
	status = open_nt(device_path, GENERIC_WRITE, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE, &handle);
	if (NT_SUCCESS(status))
		(void)NtClose(handle);
	check("a 256-character name: STATUS_OBJECT_NAME_INVALID", status == STATUS_OBJECT_NAME_INVALID);

	// The module's path: a 23-character file name below the four directories reaches 519 bytes, the longest it
	// takes, and a 24-character one is refused.
	snprintf(device_path, sizeof device_path, "%sexfat_check", DEVICE_ROOT);
	bool made = true;
	for (unsigned level = 0; level < DEEP_LEVELS; level++) {
		memset(name, 'a' + (int)level, DEEP_NAME_CHARS);
		name[DEEP_NAME_CHARS] = 0;
		snprintf(device_path + strlen(device_path), sizeof device_path - strlen(device_path), "\\%s", name);
		status = open_nt(
			device_path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_CREATE, FILE_DIRECTORY_FILE, &handle);
		if (NT_SUCCESS(status))
			(void)NtClose(handle);
		made = made && NT_SUCCESS(status);
	}
	check("four 120-character directories created, 495 bytes under the volume", made);
	const size_t deep_length = strlen(device_path);
	memset(name, 'f', 24);
	name[23] = 0;
	snprintf(device_path + deep_length, sizeof device_path - deep_length, "\\%s", name);
	status = open_nt(device_path, GENERIC_WRITE, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE, &handle);
	check("a file at a 519-byte path created", NT_SUCCESS(status));
	if (NT_SUCCESS(status)) {
		(void)NtWriteFile(handle, 0, NULL, NULL, &io, "DEEP", 4, NULL);
		(void)NtClose(handle);
	}
	status = open_nt(device_path, GENERIC_READ, FILE_SHARE_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &handle);
	memset(back, 0, 4);
	check("it reads back", NT_SUCCESS(status) && read_nt(handle, back, 4, 0) && memcmp(back, "DEEP", 4) == 0);
	if (NT_SUCCESS(status))
		(void)NtClose(handle);
	check("delete the file at 519 bytes", NT_SUCCESS(delete_nt(device_path, FILE_NON_DIRECTORY_FILE)));
	name[23] = 'f';
	name[24] = 0;
	snprintf(device_path + deep_length, sizeof device_path - deep_length, "\\%s", name);
	status = open_nt(device_path, GENERIC_WRITE, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE, &handle);
	if (NT_SUCCESS(status))
		(void)NtClose(handle);
	check("a 520-byte path: STATUS_OBJECT_NAME_INVALID", status == STATUS_OBJECT_NAME_INVALID);
	// Through xsdk, which takes either separator: the directories' path works, the 520-byte file path meets the
	// module's refusal, and a 40-character name below them, 536 under the volume, passes xsdk's 548 resolved.
	device_path[deep_length] = 0;
	snprintf(path, sizeof path, "%s%s", FOLDER, device_path + strlen(DEVICE_ROOT) + strlen("exfat_check"));
	check("the deep path through xsdk: a directory", stat(path, &info) == 0 && S_ISDIR(info.st_mode));
	const size_t deep_path_length = strlen(path);
	snprintf(path + deep_path_length, sizeof path - deep_path_length, "/%s", name);
	open_fails("the 520-byte path through xsdk: EINVAL, the module's refusal", path, O_RDONLY, EINVAL);
	memset(name, 'f', 40);
	name[40] = 0;
	snprintf(path + deep_path_length, sizeof path - deep_path_length, "/%s", name);
	open_fails("a 536-byte path through xsdk: ENAMETOOLONG, its own limit", path, O_RDONLY, ENAMETOOLONG);
	// Removed bottom up through the kernel, each once empty.
	bool removed = true;
	for (unsigned level = 0; level < DEEP_LEVELS; level++) {
		removed = removed && NT_SUCCESS(delete_nt(device_path, FILE_DIRECTORY_FILE));
		*strrchr(device_path, '\\') = 0;
	}
	check("the four directories removed", removed);
	memset(name, 'a', DEEP_NAME_CHARS);
	name[DEEP_NAME_CHARS] = 0;
	snprintf(path, sizeof path, "%s/%s", FOLDER, name);
	check_errno("the first of them is gone", stat(path, &info), ENOENT);
}

static void check_attributes(void)
{
	struct stat info;
	static const char ro[] = "usb0:/exfat_check/ro.txt";
	check("ro.txt written", write_small(ro, "RO", 2));
	check_ok("chmod 0444", chmod(ro, 0444));
	check("stat shows no owner write bit", stat(ro, &info) == 0 && !(info.st_mode & S_IWUSR));
	open_fails("open a read-only file for writing: EACCES", ro, O_WRONLY, EACCES);
	const int fd = open(ro, O_RDONLY);
	check("a read-only file still opens for reading", fd >= 0);
	if (fd >= 0)
		(void)close(fd);
	check_errno("unlink a read-only file: EPERM", unlink(ro), EPERM);
	check_ok("chmod 0644", chmod(ro, 0644));
	check("stat shows the write bit again", stat(ro, &info) == 0 && (info.st_mode & S_IWUSR));
	check_ok("unlink ro.txt", unlink(ro));
}

static void check_free_space(void)
{
	struct statvfs before;
	struct statvfs after;
	check_ok("statvfs before a 4 MiB file", statvfs(ROOT, &before));
	int fd = open("usb0:/exfat_check/four.bin", O_WRONLY | O_CREAT | O_EXCL, 0666);
	bool written = fd >= 0;
	for (unsigned k = 0; written && k < FOUR_MIB_CHUNKS; k++) {
		fill_chunk(scratch, k);
		written = write(fd, scratch, CHUNK_BYTES) == CHUNK_BYTES;
	}
	check("four.bin written", written && fsync(fd) == 0 && close(fd) == 0);
	check_ok("statvfs after", statvfs(ROOT, &after));
	const uint64_t taken = (uint64_t)(before.f_bfree - after.f_bfree) * before.f_frsize;
	xsdk_log_write("  free clusters %lu -> %lu, %lu bytes each", (unsigned long)before.f_bfree,
		(unsigned long)after.f_bfree, (unsigned long)before.f_frsize);
	check("4 MiB took 4 MiB of free space", taken == (uint64_t)FOUR_MIB_CHUNKS << 20);
	check_ok("unlink four.bin", unlink("usb0:/exfat_check/four.bin"));
	check("the free space came back", statvfs(ROOT, &after) == 0 && after.f_bfree == before.f_bfree);
}

static void check_kernel_calls(void)
{
	// Share access: an open that shares nothing keeps a second one out until it closes.
	HANDLE first;
	HANDLE second;
	NTSTATUS status = open_nt(DEVICE_PATTERN, GENERIC_READ, 0, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &first);
	NTSTATUS status2 = open_nt(DEVICE_PATTERN, GENERIC_READ, 0, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &second);
	check("a second open of an unshared file: STATUS_SHARING_VIOLATION",
		NT_SUCCESS(status) && status2 == STATUS_SHARING_VIOLATION);
	if (NT_SUCCESS(status2))
		(void)NtClose(second);
	if (NT_SUCCESS(status))
		(void)NtClose(first);
	status2 = open_nt(DEVICE_PATTERN, GENERIC_READ, 0, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &second);
	check("after the close it opens", NT_SUCCESS(status2));
	if (NT_SUCCESS(status2))
		(void)NtClose(second);
	status = open_nt(DEVICE_PATTERN, GENERIC_READ, FILE_SHARE_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &first);
	status2 = open_nt(DEVICE_PATTERN, GENERIC_READ, FILE_SHARE_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &second);
	check("two opens sharing read coexist", NT_SUCCESS(status) && NT_SUCCESS(status2));
	if (NT_SUCCESS(status2))
		(void)NtClose(second);
	if (NT_SUCCESS(status))
		(void)NtClose(first);

	// The offset the kernel keeps for a synchronous file: two reads with none given follow each other.
	status = open_nt(
		DEVICE_PATTERN, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, FILE_NON_DIRECTORY_FILE, &first);
	check("NtCreateFile of pattern.bin", NT_SUCCESS(status));
	IO_STATUS_BLOCK io = { 0, 0 };
	memset(back, 0, 200);
	status = NtReadFile(first, 0, NULL, NULL, &io, back, 100, NULL);
	status2 = NtReadFile(first, 0, NULL, NULL, &io, back + 100, 100, NULL);
	check("two reads without an offset read bytes 0-99 then 100-199",
		NT_SUCCESS(status) && NT_SUCCESS(status2) && io.information == 100 && memcmp(back, chunk, 200) == 0);
	uint64_t position = 0;
	status = NtQueryInformationFile(first, &io, &position, sizeof position, FILE_POSITION_INFORMATION_CLASS);
	check("the position then reads 200", NT_SUCCESS(status) && position == 200);
	xsdk_log_write("  position: status 0x%08X, %llu", (unsigned)status, (unsigned long long)position);
	(void)NtClose(first);
}

static void check_big_file(void)
{
	struct statvfs before;
	struct statvfs after;
	check_ok("statvfs before big.bin", statvfs(ROOT, &before));
	xsdk_log_write("big.bin: writing %u MiB", BIG_CHUNKS);
	const int fd = open(BIG, O_WRONLY | O_CREAT | O_EXCL, 0666);
	check("create big.bin", fd >= 0);
	const uint64_t start = now_ms();
	bool written = fd >= 0;
	uint32_t k;
	for (k = 0; written && k < BIG_CHUNKS; k++) {
		fill_chunk(scratch, k);
		written = write(fd, scratch, CHUNK_BYTES) == CHUNK_BYTES;
		if (written && (k + 1) % 1024 == 0) {
			const uint64_t elapsed = now_ms() - start;
			xsdk_log_write("big.bin: %u MiB in %llu ms, %llu MiB/s", k + 1, (unsigned long long)elapsed,
				(unsigned long long)((uint64_t)(k + 1) * 1000 / (elapsed ? elapsed : 1)));
		}
	}
	const uint64_t elapsed = now_ms() - start;
	if (!written)
		xsdk_log_write("  chunk %u failed, errno %d", k, errno);
	check("write 4 GiB + 1 MiB sequentially", written);
	xsdk_log_write("RESULT big.bin write: %u MiB in %llu ms, %llu MiB/s", k, (unsigned long long)elapsed,
		(unsigned long long)((uint64_t)k * 1000 / (elapsed ? elapsed : 1)));
	check_ok("fsync big.bin", fsync(fd));
	check_ok("close big.bin", close(fd));
	uint64_t size;
	NTSTATUS status = size_nt(DEVICE_BIG, &size);
	check("big.bin is 4 GiB + 1 MiB", NT_SUCCESS(status) && size == BIG_BYTES);
	xsdk_log_write("  size: status 0x%08X, %llu", (unsigned)status, (unsigned long long)size);

	HANDLE handle;
	status = open_nt(DEVICE_BIG, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN,
		FILE_NON_DIRECTORY_FILE, &handle);
	check("NtCreateFile of big.bin", NT_SUCCESS(status));
	fill_chunk(scratch, 0);
	check("big.bin at 0", read_nt(handle, back, 4096, 0) && memcmp(back, scratch, 4096) == 0);
	// Across 4 GiB: chunk 4095's last 512 bytes, then chunk 4096's first.
	fill_chunk(scratch, 4095);
	memcpy(chunk, scratch + CHUNK_BYTES - 512, 512);
	fill_chunk(scratch, 4096);
	memcpy(chunk + 512, scratch, 512);
	check("big.bin across 4 GiB", read_nt(handle, back, 1024, FOUR_GIB - 512) && memcmp(back, chunk, 1024) == 0);
	check("big.bin at 4 GiB", read_nt(handle, back, 4096, FOUR_GIB) && memcmp(back, scratch, 4096) == 0);
	check("big.bin's tail",
		read_nt(handle, back, 4096, BIG_BYTES - 4096) && memcmp(back, scratch + CHUNK_BYTES - 4096, 4096) == 0);
	IO_STATUS_BLOCK io = { 0, 0 };
	status = NtReadFile(handle, 0, NULL, NULL, &io, back, 16, &BIG_BYTES);
	check("a read at its end: STATUS_END_OF_FILE", status == STATUS_END_OF_FILE);
	const FILE_END_OF_FILE_INFORMATION cut = { BIG_CUT_BYTES };
	status = NtSetInformationFile(handle, &io, &cut, sizeof cut, FILE_END_OF_FILE_INFORMATION_CLASS);
	check("truncate big.bin to 4 GiB + 4096", NT_SUCCESS(status));
	check("big.bin at 4 GiB + 4000 still matches",
		read_nt(handle, back, 96, FOUR_GIB + 4000) && memcmp(back, scratch + 4000, 96) == 0);
	status = NtReadFile(handle, 0, NULL, NULL, &io, back, 16, &BIG_CUT_BYTES);
	check("a read at the new end: STATUS_END_OF_FILE", status == STATUS_END_OF_FILE);
	(void)NtClose(handle);
	status = size_nt(DEVICE_BIG, &size);
	check("big.bin is 4 GiB + 4096", NT_SUCCESS(status) && size == BIG_CUT_BYTES);
	check_ok("statvfs after big.bin", statvfs(ROOT, &after));
	const uint64_t taken = (uint64_t)(before.f_bfree - after.f_bfree) * before.f_frsize;
	const uint64_t expected = (BIG_CUT_BYTES + before.f_frsize - 1) / before.f_frsize * before.f_frsize;
	xsdk_log_write("  free clusters %lu -> %lu: %llu bytes taken, %llu expected", (unsigned long)before.f_bfree,
		(unsigned long)after.f_bfree, (unsigned long long)taken, (unsigned long long)expected);
	check("big.bin takes its clusters and no more", taken == expected);
	// chunk held pattern.bin's bytes; nothing reads it after this.
}

int main(void)
{
	(void)xsdk_splash_show(SPLASH_MS);
	xsdk_log_write("exfat_check: milestone 77 step 3, kernel %u.%u.%u.%u", XboxKrnlVersion->major,
		XboxKrnlVersion->minor, XboxKrnlVersion->build, XboxKrnlVersion->qfe);
	if (XboxKrnlVersion->build != KERNEL_BUILD) {
		xsdk_log_write("exfat_check: the module is kernel %u's; nothing to check", KERNEL_BUILD);
		return 1;
	}
	if (!mounted_exfat()) {
		summary();
		return 1;
	}
	struct stat info;
	remove_tree(FOLDER, 0);
	(void)rmdir(FOLDER);
	check_errno("exfat_check gone before the run", stat(FOLDER, &info), ENOENT);
	check_ok("mkdir exfat_check", mkdir(FOLDER, 0777));
	check_errno("mkdir exfat_check again: EEXIST", mkdir(FOLDER, 0777), EEXIST);
	check_errno("mkdir under a missing parent: ENOENT", mkdir("usb0:/exfat_check/missing/sub", 0777), ENOENT);
	check("stat exfat_check: a directory", stat(FOLDER, &info) == 0 && S_ISDIR(info.st_mode));
	check_pattern_file();
	check_names();
	check_long_names();
	check_attributes();
	check_free_space();
	check_kernel_calls();
	check_big_file();

	// verify.bin for the PC, then what the PC needn't see goes.
	fill_chunk(scratch, VERIFY_CHUNK);
	check("verify.bin written", write_small("usb0:/exfat_check/verify.bin", scratch, CHUNK_BYTES));
	check_ok("unlink pattern.bin", unlink(PATTERN));
	remove_tree("usb0:/exfat_check/listing", 0);
	check_ok("rmdir listing", rmdir("usb0:/exfat_check/listing"));
	check("exfat_check holds big.bin and verify.bin alone",
		list_sorted(FOLDER, 0) == 2 && strcmp(names[0][0], "big.bin") == 0 && strcmp(names[0][1], "verify.bin") == 0);
	summary();
	return failed_count ? 1 : 0;
}
