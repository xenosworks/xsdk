#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include "tests.h"
#include "xsdk/log.h"

enum {
	PATH_BYTES = 64,
	HOLD_MS = 50,
	LOG_COST_LINES = 100,
	LOG_COST_DBGPRINT_LINES = 10,
	// About 3.5 times a line to the file and over the network, 0.31 ms and up to 0.13 more (docs/HARDWARE.md, "The log
	// over the network, milestone 53"); DbgPrint's 3 to 7 ms, which every line paid before, fails it.
	LOG_COST_BAR_US = 1500,
	CHECK_NAME_BYTES = 64,
	// A file of 4 allocation units and a byte takes 5, as a file takes whole units; its folder may take 1 more for the
	// new name.
	STATVFS_FILE_UNITS = 4,
	STATVFS_UNITS_TAKEN = STATVFS_FILE_UNITS + 1,
	STATVFS_FOLDER_UNITS = 1,
	WRITE_CHUNK_BYTES = 16384,
	// NT's MAX_PATH; ExLoadedImageName holds up to 256 bytes.
	DRIVE_PATH_BYTES = 260,
	DRIVE_CHECK_NAME_BYTES = 128,
	QUERY_BYTES = 1024,
};

// game: is the launch folder on the console; Xenia mounts it read-only but lets titles write to cache:.
static const char *const directories[] = { "game:\\", "cache:\\" };
static const char FILE_NAME[] = "xsdk_file_test.txt";

// The path of another file in path's folder, or of the folder itself for an empty name.
static void sibling_path(char other[PATH_BYTES], const char *path, const char *name)
{
	strcpy(other, path);
	strcpy(strrchr(other, '\\') + 1, name);
}

// No printf here: Xenia mangles formatted text, and this test is about file IO.
static FILE *create_test_file(char *path)
{
	for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); i++) {
		strcpy(path, directories[i]);
		strcat(path, FILE_NAME);
		FILE *file = fopen(path, "w");
		if (file)
			return file;
	}
	return NULL;
}

static long file_size(const char *path)
{
	FILE *file = fopen(path, "r");
	if (!file)
		return -1;
	long size = -1;
	if (fseek(file, 0, SEEK_END) == 0)
		size = ftell(file);
	fclose(file);
	return size;
}

// The test file holds "line one\nline 2\nline three\n": "line 2" starts at byte 9, and the file is 27 bytes.
static void check_pread(const char *path)
{
	char bytes[8] = "";
	const int fd = open(path, O_RDONLY);
	const bool read_at_offset = fd >= 0 && pread(fd, bytes, 6, 9) == 6 && memcmp(bytes, "line 2", 6) == 0;
	const bool position_kept = fd >= 0 && lseek(fd, 0, SEEK_CUR) == 0;
	const bool nothing_at_end = fd >= 0 && pread(fd, bytes, 1, 27) == 0;
	if (fd >= 0)
		close(fd);
	check("pread: reads at an offset and leaves the position", read_at_offset && position_kept);
	check("pread: reads nothing at the end", nothing_at_end);
}

static volatile bool thread_has_lock;

static void *lock_file(void *file)
{
	flockfile(file);
	thread_has_lock = true;
	funlockfile(file);
	return NULL;
}

static void hold(void)
{
	const struct timespec delay = { .tv_nsec = HOLD_MS * 1000000 };
	nanosleep(&delay, NULL);
}

// The standard streams are each thread's own, so the other thread is handed this one's stdout.
static void check_flockfile(void)
{
	FILE *file = stdout;
	flockfile(file);
	flockfile(file);
	pthread_t thread;
	if (pthread_create(&thread, NULL, lock_file, file) != 0) {
		funlockfile(file);
		funlockfile(file);
		check("flockfile: another thread waits while the lock is held, twice and then once", false);
		return;
	}
	hold();
	const bool waited_while_held_twice = !thread_has_lock;
	funlockfile(file);
	hold();
	const bool waited_while_held_once = !thread_has_lock;
	funlockfile(file);
	pthread_join(thread, NULL);
	check("flockfile: another thread waits while the lock is held, twice and then once",
		waited_while_held_twice && waited_while_held_once && thread_has_lock);
}

// The log, opened again for reading while the title has it open for writing, as a tool on the console reads it while a
// title runs. Only the console can fail this: Xenia ignores share access (xboxkrnl/xboxkrnl_io.cc:102-145).
static void check_log_readable(void)
{
	const char *const path = xsdk_log_path();
	if (!path) {
		check("log read while it's open", false);
		return;
	}
	const ANSI_STRING name = {
		.length = (uint16_t)strlen(path),
		.maximum_length = (uint16_t)(strlen(path) + 1),
		.pointer = path,
	};
	const OBJECT_ATTRIBUTES attributes = { .root_directory = path[0] == '\\' ? 0 : OB_DOS_DEVICES, .name = &name };
	IO_STATUS_BLOCK io_status;
	HANDLE handle;
	const NTSTATUS status =
		NtCreateFile(&handle, GENERIC_READ | SYNCHRONIZE, &attributes, &io_status, NULL, FILE_ATTRIBUTE_NORMAL,
			FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
	// The log's first line is "log: " and its path (src/log.c).
	char start[6] = "";
	if (NT_SUCCESS(status)) {
		const uint64_t offset = 0;
		(void)NtReadFile(handle, 0, NULL, NULL, &io_status, start, sizeof(start) - 1, &offset);
		(void)NtClose(handle);
	}
	xsdk_log_write(
		"files: the log opened for reading while it's open: 0x%08X, starting \"%s\"", (unsigned)status, start);
	check("log read while it's open", NT_SUCCESS(status) && strcmp(start, "log: ") == 0);
}

// POSIX lets a file be open more than once at a time, to write and to read.
static void check_open_twice(const char *path)
{
	const int writer = open(path, O_RDWR);
	const int reader = open(path, O_RDONLY);
	char bytes[8] = "";
	const bool both = writer >= 0 && reader >= 0 && pread(reader, bytes, 6, 9) == 6 && memcmp(bytes, "line 2", 6) == 0;
	if (reader >= 0)
		close(reader);
	if (writer >= 0)
		close(writer);
	check("open: a file opens twice, to write and to read", both);
}

// pwrite of "LINE 2" over "line 2", with the position moved to 4 first: pread reads it back, and the position and the
// file's size are as they were.
static void check_pwrite(const char *path)
{
	const int fd = open(path, O_RDWR);
	char bytes[8] = "";
	const bool written = fd >= 0 && lseek(fd, 4, SEEK_SET) == 4 && pwrite(fd, "LINE 2", 6, 9) == 6;
	const bool read_back = written && pread(fd, bytes, 6, 9) == 6 && memcmp(bytes, "LINE 2", 6) == 0;
	const bool position_kept = fd >= 0 && lseek(fd, 0, SEEK_CUR) == 4;
	if (fd >= 0)
		close(fd);
	check("pwrite: writes at an offset, read back by pread, the position and size unchanged",
		read_back && position_kept && file_size(path) == 27);
}

// The file's size, through the descriptor where it's open, else by its path; -1 where neither answers.
static long long current_size(int fd, const char *path)
{
	struct stat info;
	const int result = fd >= 0 ? fstat(fd, &info) : stat(path, &info);
	return result == 0 ? (long long)info.st_size : -1;
}

static void to_hex(char *hex, const unsigned char *bytes, size_t length_bytes)
{
	for (size_t i = 0; i < length_bytes; i++)
		(void)snprintf(hex + 2 * i, 3, "%02X", bytes[i]);
}

// ftruncate, or truncate by the path with the file closed, takes "line one\nline 2\n" to its first line, then to 16
// bytes again, the 7 new ones zero. Every step's result is logged, the bytes read back in hex, for the console to say
// which step fails.
static bool shorten_and_lengthen(const char *path, bool by_path)
{
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	const ssize_t written = write(fd, "line one\nline 2\n", 16);
	// truncate opens the file sharing nothing, so it isn't open here meanwhile.
	if (by_path && fd >= 0) {
		close(fd);
		fd = -1;
	}
	errno = 0;
	const int shortened = by_path ? truncate(path, 9) : ftruncate(fd, 9);
	const int shorten_errno = errno;
	const long long short_size = current_size(fd, path);
	errno = 0;
	const int lengthened = by_path ? truncate(path, 16) : ftruncate(fd, 16);
	const int lengthen_errno = errno;
	const long long long_size = current_size(fd, path);
	if (by_path)
		fd = open(path, O_RDONLY);
	unsigned char bytes[16];
	memset(bytes, 'x', sizeof(bytes));
	const ssize_t read_bytes = pread(fd, bytes, sizeof(bytes), 0);
	if (fd >= 0)
		close(fd);
	(void)unlink(path);
	char hex[sizeof(bytes) * 2 + 1];
	to_hex(hex, bytes, sizeof(bytes));
	xsdk_log_write(
		"files: %s: wrote %d; to 9 bytes %d (errno %d), size %lld; to 16 bytes %d (errno %d), size %lld; "
		"pread %d: %s",
		by_path ? "truncate" : "ftruncate", (int)written, shortened, shorten_errno, short_size, lengthened,
		lengthen_errno, long_size, (int)read_bytes, hex);
	return written == 16 && shortened == 0 && short_size == 9 && lengthened == 0 && long_size == 16 &&
		read_bytes == 16 && memcmp(bytes, "line one\n\0\0\0\0\0\0\0", sizeof(bytes)) == 0;
}

static void check_ftruncate(const char *path)
{
	const bool by_fd = shorten_and_lengthen(path, false);
	const bool by_path = shorten_and_lengthen(path, true);
	check("ftruncate and truncate: shorten a file, then lengthen it with zeros", by_fd && by_path);
}

// A write past the end of a file cut short leaves the gap reading zeros, not the bytes cut off. The hard disk brought
// those back when ftruncate set the end alone (results/2026-10-20-milestone-54/step2_xsdk_test.log); whether a write
// past the end does too is for the console to say, the bytes logged in hex.
static void check_write_past_end(const char *path)
{
	const int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	const bool cut = fd >= 0 && write(fd, "line one\nline 2\n", 16) == 16 && ftruncate(fd, 9) == 0;
	const bool past = cut && pwrite(fd, "!", 1, 16) == 1;
	unsigned char bytes[17];
	memset(bytes, 'x', sizeof(bytes));
	const ssize_t read_bytes = pread(fd, bytes, sizeof(bytes), 0);
	if (fd >= 0)
		close(fd);
	(void)unlink(path);
	char hex[sizeof(bytes) * 2 + 1];
	to_hex(hex, bytes, sizeof(bytes));
	xsdk_log_write("files: a byte written at 16 past a 9-byte end: pread %d: %s", (int)read_bytes, hex);
	check("pwrite: past the end, the gap before it reads zeros",
		past && read_bytes == 17 && memcmp(bytes, "line one\n\0\0\0\0\0\0\0!", sizeof(bytes)) == 0);
}

// fsync and fdatasync flush a file; a socket has nothing to flush, so fails with EINVAL, as POSIX has it.
static void check_fsync(const char *path)
{
	const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	const bool file_synced = fd >= 0 && write(fd, "x", 1) == 1 && fsync(fd) == 0 && fdatasync(fd) == 0;
	if (fd >= 0)
		close(fd);
	(void)unlink(path);
	const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
	const bool socket_refused = socket_fd >= 0 && fsync(socket_fd) != 0 && errno == EINVAL;
	if (socket_fd >= 0)
		close(socket_fd);
	check("fsync: flushes a file and fails on a socket", file_synced && socket_refused);
}

static bool write_zeros(int fd, unsigned long length_bytes)
{
	static const char zeros[WRITE_CHUNK_BYTES];
	while (length_bytes > 0) {
		const size_t chunk_bytes = length_bytes < sizeof(zeros) ? length_bytes : sizeof(zeros);
		if (write(fd, zeros, chunk_bytes) != (ssize_t)chunk_bytes)
			return false;
		length_bytes -= chunk_bytes;
	}
	return true;
}

// statvfs of the test file's folder, game: on the console, before and after a file is written and synced, and
// fstatvfs of the file.
static void check_statvfs(const char *folder, const char *path)
{
	struct statvfs before = { 0 };
	struct statvfs by_fd;
	struct statvfs after;
	const bool read_before = statvfs(folder, &before) == 0 && before.f_bsize > 0 && before.f_blocks >= before.f_bfree;
	const unsigned long file_bytes = STATVFS_FILE_UNITS * before.f_bsize + 1;
	const int fd = read_before ? open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
	const bool synced = fd >= 0 && write_zeros(fd, file_bytes) && fsync(fd) == 0 && fstatvfs(fd, &by_fd) == 0 &&
		by_fd.f_bsize == before.f_bsize;
	if (fd >= 0)
		close(fd);
	const bool read_after = synced && statvfs(folder, &after) == 0;
	(void)unlink(path);
	const fsblkcnt_t taken = read_after ? before.f_bfree - after.f_bfree : 0;
	xsdk_log_write("files: statvfs(%s): %lu-byte units, %llu of %llu free; a file of %lu bytes took %llu", folder,
		before.f_bsize, (unsigned long long)before.f_bfree, (unsigned long long)before.f_blocks, file_bytes,
		(unsigned long long)taken);
	check("statvfs: a written file takes its allocation units from the free ones, fstatvfs agreeing on the unit",
		read_after && taken >= STATVFS_UNITS_TAKEN && taken <= STATVFS_UNITS_TAKEN + STATVFS_FOLDER_UNITS);
}

// The entries in a folder, or -1 with errno set where it doesn't open.
static int count_entries(const char *folder)
{
	DIR *const dir = opendir(folder);
	if (!dir)
		return -1;
	int entries = 0;
	while (readdir(dir))
		entries++;
	(void)closedir(dir);
	return entries;
}

// hdd: lists its root and holds the .xex game: runs from, the same file through both, where it's on the hard disk.
static void check_hdd(void)
{
	static const char partition[] = "\\Device\\Harddisk0\\Partition1\\";
	const char *const image = ExLoadedImageName ? ExLoadedImageName : "";
	if (strncasecmp(image, partition, sizeof(partition) - 1) != 0) {
		xsdk_log_write("drives: SKIP the hdd: check, as the .xex runs from %s, not the hard disk", image);
		return;
	}
	char on_hdd[DRIVE_PATH_BYTES];
	char in_game[DRIVE_PATH_BYTES];
	(void)snprintf(on_hdd, sizeof(on_hdd), "hdd:\\%s", image + sizeof(partition) - 1);
	(void)snprintf(in_game, sizeof(in_game), "game:\\%s", strrchr(image, '\\') + 1);
	const int entries = count_entries("hdd:/");
	struct stat by_hdd;
	struct stat by_game;
	const bool same = stat(on_hdd, &by_hdd) == 0 && stat(in_game, &by_game) == 0 && by_hdd.st_size == by_game.st_size &&
		by_hdd.st_mtime == by_game.st_mtime;
	xsdk_log_write("drives: hdd:\\ lists %d entries; %s and %s %s", entries, on_hdd, in_game,
		same ? "are the same file" : "aren't the same file");
	check("drives: hdd: lists its root and holds the .xex game: runs from", entries > 0 && same);
}

// The entries in a device's root through the kernel alone, as xsdk's paths can't name it with its closing backslash;
// -1 where it doesn't open, its status in status.
static int count_device_entries(const char *root, NTSTATUS *status)
{
	const ANSI_STRING name = {
		.length = (uint16_t)strlen(root), .maximum_length = (uint16_t)(strlen(root) + 1), .pointer = root
	};
	const OBJECT_ATTRIBUTES attributes = { .root_directory = 0, .name = &name };
	IO_STATUS_BLOCK io_status;
	HANDLE handle;
	*status = NtCreateFile(&handle, GENERIC_READ | SYNCHRONIZE, &attributes, &io_status, NULL, FILE_ATTRIBUTE_NORMAL,
		FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_DIRECTORY_FILE);
	if (!NT_SUCCESS(*status))
		return -1;
	static const ANSI_STRING all = { .length = 1, .maximum_length = 2, .pointer = "*" };
	static uint64_t buffer[QUERY_BYTES / sizeof(uint64_t)];
	int entries = 0;
	for (bool first = true;; first = false) {
		// A FAT32 stick's driver leaves next_entry_offset as it finds it (docs/HARDWARE.md), so 0 ends the entries.
		memset(buffer, 0, sizeof(buffer));
		if (!NT_SUCCESS(NtQueryDirectoryFile(handle, 0, NULL, NULL, &io_status, (FILE_DIRECTORY_INFORMATION *)buffer,
				sizeof(buffer), first ? &all : NULL, first)))
			break;
		for (uint32_t offset = 0;;) {
			entries++;
			const uint32_t next =
				((const FILE_DIRECTORY_INFORMATION *)((const char *)buffer + offset))->next_entry_offset;
			if (next == 0 || offset + next >= sizeof(buffer))
				break;
			offset += next;
		}
	}
	(void)NtClose(handle);
	return entries;
}

// A drive lists its root as its device does and gives statvfs where there's something in it, and fails both with
// ENOENT where its device doesn't open.
static void check_drive(const char *drive, const char *device_root)
{
	char root[DRIVE_PATH_BYTES];
	(void)snprintf(root, sizeof(root), "%s/", drive);
	errno = 0;
	const int entries = count_entries(root);
	const int open_errno = errno;
	struct statvfs info = { 0 };
	errno = 0;
	const int sized = statvfs(drive, &info);
	const int statvfs_errno = errno;
	NTSTATUS device_status;
	const int device_entries = count_device_entries(device_root, &device_status);
	xsdk_log_write(
		"drives: %s lists %d entries (errno %d), %s %d (0x%08X); statvfs %d (errno %d): %lu-byte units, "
		"%llu of %llu free",
		drive, entries, open_errno, device_root, device_entries, (unsigned)device_status, sized, statvfs_errno,
		info.f_bsize, (unsigned long long)info.f_bfree, (unsigned long long)info.f_blocks);
	char name[DRIVE_CHECK_NAME_BYTES];
	(void)snprintf(name, sizeof(name), "drives: %s lists its root as %s does, or fails with ENOENT where it does",
		drive, device_root);
	const bool listed = device_entries >= 0 && entries == device_entries && sized == 0 && info.f_bsize > 0;
	const bool empty = device_entries < 0 && entries < 0 && open_errno == ENOENT && statvfs_errno == ENOENT;
	check(name, listed || empty);
}

// The test file, renamed over another, takes its place, and its own name is gone.
static void check_rename(const char *path)
{
	char other[PATH_BYTES];
	sibling_path(other, path, "xsdk_rename_test.txt");
	FILE *file = fopen(other, "w");
	const bool made = file && fputs("old\n", file) >= 0 && fclose(file) == 0;
	const bool renamed = made && rename(path, other) == 0;
	check("rename: a file renamed over another takes its place",
		renamed && file_size(other) == 27 && file_size(path) < 0);
	(void)unlink(other);
}

// The nanoseconds a line of the log takes over lines of them.
static long long time_log_lines(unsigned lines, const char *how)
{
	struct timespec start, end;
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	for (unsigned line = 0; line < lines; line++)
		xsdk_log_write("files: a line to time, %u of %u", line + 1, lines);
	(void)clock_gettime(CLOCK_MONOTONIC, &end);
	const long long elapsed_ns =
		(long long)(end.tv_sec - start.tv_sec) * 1000000000LL + (long long)(end.tv_nsec - start.tv_nsec);
	const long long line_ns = elapsed_ns / lines;
	xsdk_log_write("files: %u log lines %s took %lld us, %lld.%03lld us a line", lines, how, elapsed_ns / 1000,
		line_ns / 1000, line_ns % 1000);
	return line_ns;
}

// What a line of the log costs, to the file and, with net_log.txt beside the .xex, over the network too
// (docs/scope/53-network-log.md); then a few with DbgPrint on as well, for its cost, which no bar holds.
static void check_log_cost(void)
{
	const long long line_ns = time_log_lines(LOG_COST_LINES, "as the title runs");
	char name[CHECK_NAME_BYTES];
	(void)snprintf(name, sizeof name, "log: a line costs under %u us", (unsigned)LOG_COST_BAR_US);
	check(name, line_ns < LOG_COST_BAR_US * 1000LL);
	const bool dbgprint = xsdk_log_set_dbgprint(true);
	(void)time_log_lines(LOG_COST_DBGPRINT_LINES, "with DbgPrint on");
	(void)xsdk_log_set_dbgprint(dbgprint);
}

// basename and dirname of each path, as POSIX gives them, and for a drive's path with \.
static void check_path_parts(void)
{
	static const char *const cases[][3] = {
		{ "usr/lib/", "lib", "usr" },
		{ "/usr", "usr", "/" },
		{ "file", "file", "." },
		{ "/", "/", "/" },
		{ "", ".", "." },
		{ "game:\\saves\\slot1.dat", "slot1.dat", "game:\\saves" },
	};
	bool all = true;
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		char base_copy[PATH_BYTES];
		char dir_copy[PATH_BYTES];
		(void)snprintf(base_copy, sizeof(base_copy), "%s", cases[i][0]);
		(void)snprintf(dir_copy, sizeof(dir_copy), "%s", cases[i][0]);
		const char *const base = basename(base_copy);
		const char *const dir = dirname(dir_copy);
		const bool right = strcmp(base, cases[i][1]) == 0 && strcmp(dir, cases[i][2]) == 0;
		if (!right)
			xsdk_log_write("files: \"%s\": basename \"%s\", dirname \"%s\"", cases[i][0], base, dir);
		all &= right;
	}
	check("files: basename and dirname split paths as POSIX has them, a drive's with \\ too", all);
}

void test_files(void)
{
	check_path_parts();
	check_log_readable();
	check_log_cost();

	char path[PATH_BYTES];
	FILE *file = create_test_file(path);
	if (!file) {
		check("file written", false);
		return;
	}
	xsdk_log_write("files: %s", path);
	const bool written = fputs("line one\nline 2\n", file) >= 0;
	check("file written", fclose(file) == 0 && written);

	char line_one[32] = "";
	char line_two[32] = "";
	file = fopen(path, "r");
	if (file) {
		fgets(line_one, sizeof(line_one), file);
		fgets(line_two, sizeof(line_two), file);
		fclose(file);
	}
	check("file read back", strcmp(line_one, "line one\n") == 0 && strcmp(line_two, "line 2\n") == 0);
	check("file size from seek", file_size(path) == 16);

	file = fopen(path, "a");
	const bool appended = file && fputs("line three\n", file) >= 0 && fclose(file) == 0;
	check("file append", appended && file_size(path) == 27);
	check_pread(path);
	check_open_twice(path);
	check_pwrite(path);
	check_flockfile();
	check_rename(path);

	char folder[PATH_BYTES];
	char other[PATH_BYTES];
	sibling_path(folder, path, "");
	sibling_path(other, path, "xsdk_sync_test.bin");
	check_ftruncate(other);
	check_write_past_end(other);
	check_fsync(other);
	check_statvfs(folder, other);

	check_hdd();
	check_drive("usb:", "\\Device\\Mass0\\");
	check_drive("dvd:", "\\Device\\Cdrom0\\");
	check_drive("mu:", "\\Device\\Mu0\\");
}
