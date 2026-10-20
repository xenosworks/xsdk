#include "xsdk/log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "xsdk/kernel.h"

enum {
	LOG_LINE_BYTES = 512,
	// The log's name, the title's with .log, and the folder before it.
	LOG_NAME_BYTES = 64,
	LOG_PATH_BYTES = 128,
	// A datagram of the network log: the line's sequence number, then the line.
	NET_SEQUENCE_BYTES = 4,
	NET_NOTE_BYTES = 96,
};

typedef struct {
	HANDLE root_directory;
	const char *folder;
} LogFolder;

// game: is where the title was launched from; cache: is the folder Xenia lets titles write to.
static const LogFolder log_folders[] = {
	{ OB_DOS_DEVICES, "game:\\" },
	{ 0, "\\Device\\Harddisk0\\Partition1\\" },
	{ 0, "\\Device\\Mass0\\" },
	{ OB_DOS_DEVICES, "cache:\\" },
};

// Guards log_line and the file; threads log concurrently.
static RTL_CRITICAL_SECTION log_lock;
static HANDLE log_file;
static char log_line[LOG_LINE_BYTES];
static char log_path[LOG_PATH_BYTES];

// The network log, on where net_log.txt beside the .xex names the PC as "<address> <port>": each line also goes there
// as one UDP datagram, so the title never waits on the PC, numbered so a lost one shows. -1 when off.
static int net_fd = -1;
static struct sockaddr_in net_to;
static uint32_t net_sequence;
static uint32_t net_failures;
static char net_datagram[NET_SEQUENCE_BYTES + LOG_LINE_BYTES];

// The running .xex's name, from the path the loader leaves in ExLoadedImageName, with .log for its extension; xsdk.log
// if there's no name or it doesn't fit.
static void log_name(char *name, size_t name_bytes)
{
	const char *const image = ExLoadedImageName ? ExLoadedImageName : "";
	const char *base = image;
	for (const char *c = image; *c; c++) {
		if (*c == '\\' || *c == '/' || *c == ':')
			base = c + 1;
	}
	const char *const dot = strrchr(base, '.');
	const size_t stem_bytes = dot ? (size_t)(dot - base) : strlen(base);
	if (stem_bytes == 0 || stem_bytes + sizeof ".log" > name_bytes) {
		(void)snprintf(name, name_bytes, "xsdk.log");
		return;
	}
	(void)snprintf(name, name_bytes, "%.*s.log", (int)stem_bytes, base);
}

static NTSTATUS create_file(HANDLE *handle, HANDLE root_directory, const char *path)
{
	const ANSI_STRING name = {
		.length = (uint16_t)strlen(path),
		.maximum_length = (uint16_t)(strlen(path) + 1),
		.pointer = path,
	};
	const OBJECT_ATTRIBUTES attributes = { .root_directory = root_directory, .name = &name };
	IO_STATUS_BLOCK io_status;
	// Shared for reading, so the log can be read while the title runs.
	return NtCreateFile(handle, GENERIC_WRITE | SYNCHRONIZE, &attributes, &io_status, NULL, FILE_ATTRIBUTE_NORMAL,
		FILE_SHARE_READ, FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
}

static void write_line(const char *line, size_t length_bytes)
{
	(void)DbgPrint("%s", line);
	if (net_fd >= 0) {
		const uint32_t sequence = htonl(net_sequence++);
		memcpy(net_datagram, &sequence, NET_SEQUENCE_BYTES);
		memcpy(net_datagram + NET_SEQUENCE_BYTES, line, length_bytes);
		// Not retried: the gap in the numbers shows on the PC.
		if (sendto(net_fd, net_datagram, NET_SEQUENCE_BYTES + length_bytes, 0, (const struct sockaddr *)&net_to,
				sizeof net_to) < 0)
			net_failures++;
	}
	if (!log_file)
		return;
	IO_STATUS_BLOCK io_status;
	(void)NtWriteFile(log_file, 0, NULL, NULL, &io_status, line, (uint32_t)length_bytes, NULL);
}

// Opens the network log where net_log.txt names the PC; note says what it found, for the log once the file is open.
static void open_net_log(char *note, size_t note_bytes)
{
	FILE *const file = fopen("net_log.txt", "r");
	if (!file) {
		(void)snprintf(note, note_bytes, "log: no net_log.txt beside the .xex, so not over the network");
		return;
	}
	char address[16];
	unsigned port = 0;
	in_addr_t pc = 0;
	const bool read = fscanf(file, "%15s %u", address, &port) == 2 && port <= UINT16_MAX &&
		inet_pton(AF_INET, address, &pc) == 1;
	fclose(file);
	if (!read) {
		(void)snprintf(note, note_bytes, "log: net_log.txt must hold \"<PC address> <port>\"");
		return;
	}
	const int fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		(void)snprintf(note, note_bytes, "log: no socket for the network log: errno %d", errno);
		return;
	}
	net_to = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons((in_port_t)port), .sin_addr = { pc } };
	net_fd = fd;
	(void)snprintf(note, note_bytes, "log: also over UDP to %s:%u", address, port);
}

NTSTATUS xsdk_log_open(void)
{
	RtlInitializeCriticalSection(&log_lock);
	// Before the file, so the PC gets every line, the file's failures among them.
	char net_note[NET_NOTE_BYTES];
	open_net_log(net_note, sizeof net_note);
	char name[LOG_NAME_BYTES];
	log_name(name, sizeof name);
	NTSTATUS status = STATUS_NOT_IMPLEMENTED;
	for (size_t i = 0; i < sizeof(log_folders) / sizeof(log_folders[0]); i++) {
		char path[LOG_PATH_BYTES];
		(void)snprintf(path, sizeof path, "%s%s", log_folders[i].folder, name);
		HANDLE handle;
		status = create_file(&handle, log_folders[i].root_directory, path);
		if (NT_SUCCESS(status)) {
			log_file = handle;
			strcpy(log_path, path);
			xsdk_log_write("log: %s", path);
			xsdk_log_write("log: named after %s", ExLoadedImageName ? ExLoadedImageName : "nothing");
			xsdk_log_write("%s", net_note);
			return status;
		}
		xsdk_log_write("log: NtCreateFile(%s) failed: 0x%08X", path, (unsigned)status);
	}
	xsdk_log_write("%s", net_note);
	return status;
}

const char *xsdk_log_path(void)
{
	return log_path[0] ? log_path : NULL;
}

void xsdk_log_write(const char *format, ...)
{
	RtlEnterCriticalSection(&log_lock);
	va_list args;
	va_start(args, format);
	int length = vsnprintf(log_line, sizeof(log_line) - 1, format, args);
	va_end(args);
	if (length >= 0) {
		if ((size_t)length > sizeof(log_line) - 2)
			length = sizeof(log_line) - 2;
		log_line[length] = '\n';
		log_line[length + 1] = '\0';
		write_line(log_line, (size_t)length + 1);
	}
	RtlLeaveCriticalSection(&log_lock);
}

void xsdk_log_write_bytes(const char *bytes, size_t length_bytes)
{
	RtlEnterCriticalSection(&log_lock);
	while (length_bytes) {
		size_t chunk = length_bytes < sizeof(log_line) - 1 ? length_bytes : sizeof(log_line) - 1;
		memcpy(log_line, bytes, chunk);
		log_line[chunk] = '\0';
		write_line(log_line, chunk);
		bytes += chunk;
		length_bytes -= chunk;
	}
	RtlLeaveCriticalSection(&log_lock);
}

void xsdk_log_close(void)
{
	RtlEnterCriticalSection(&log_lock);
	// The socket stays open for what's logged after; the title's end closes it.
	if (net_failures)
		xsdk_log_write("log: %u of %u lines not sent over the network", (unsigned)net_failures, (unsigned)net_sequence);
	if (log_file)
		(void)NtClose(log_file);
	log_file = 0;
	RtlLeaveCriticalSection(&log_lock);
}
