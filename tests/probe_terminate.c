#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/lock.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Milestone 54 step 4's probe (docs/scope/54-porters-files-and-system.md): what the kernel's title-terminate
// notification is given when the title is ended from outside, by Xbox Home or by the console agent sending the
// console to the dashboard. game:\terminate_case.txt names the case; without it, "read" runs.
// "run" logs its main thread, hardware thread and time base, starts two busy workers on hardware threads 2 and 4, the
// first holding a mutex 9 ms in every 10, registers the notification, and writes "line <n> time base <t>" to
// game:\terminate_out.txt every 10 ms through stdio, never flushed, until it's ended. The notification's routine writes
// game:\terminate_notify.txt through NtWriteFile, a line at a time, without newlib: the thread object, hardware thread,
// stack and TLS it runs on, against the main thread's and the workers'; how far the title's threads count in 1 ms;
// whether it can take the worker's mutex, trying for 20 ms; how many bytes stdio holds unwritten and, on one of the
// title's own threads only, whether fflush of terminate_out.txt works and how long it takes; then a line a millisecond
// with the threads' counts, until 2 s from its start, so the last line says how long it had, then takes its
// registration out. Should writes to that file fail, it goes on in terminate_late.txt in game:, else in the hard
// disk's root. "read", run after, logs those files and whether terminate_out.txt holds every line in order: as many
// as the routine saw written if its fflush worked, else up to the unwritten bytes fewer, which is what Xbox Home loses.

enum {
	// The time base's rate (docs/HARDWARE.md, "The time base runs at 49.875 MHz").
	TICKS_PER_MS = 49875,
	LINE_SPACING_MS = 10,
	LINES_PER_LOG = 1000,
	MUTEX_HELD_MS = 9,
	MUTEX_FREE_MS = 1,
	TRY_FOR_MS = 20,
	HEARTBEAT_MS = 2000,
	WORKERS = 2,
	NOTE_BYTES = 256,
	// Room at a note's end for a number or the newline.
	NOTE_SPARE_BYTES = 12,
	READ_LINE_BYTES = 256,
	LAST_LINES = 3,
	NAME_BYTES = 16,
	// The processor control region's TLS, thread object and hardware thread (xenia:src/xenia/kernel/xthread.h:74-83).
	PCR_TLS = 0x0,
	PCR_CURRENT_THREAD = 0x100,
	PCR_CURRENT_PROCESSOR = 0x10C,
	// Xenia keeps it and says nothing of what it means (xenia:src/xenia/kernel/kernel_state.cc:153-160).
	NOTIFY_PRIORITY = 0,
};

static const uint32_t worker_hardware_threads[WORKERS] = { 2, 4 };

static const char *const case_path = "game:\\terminate_case.txt";
static const char *const out_path = "game:\\terminate_out.txt";
static const char *const notify_path = "game:\\terminate_notify.txt";
// Where the routine goes on if it can't write terminate_notify.txt: game:, else the hard disk, as src/log.c falls back.
static const char *const late_paths[] = { "game:\\terminate_late.txt",
	"\\Device\\Harddisk0\\Partition1\\terminate_late.txt" };

enum { LATE_PATHS = sizeof late_paths / sizeof late_paths[0] };

static FILE *out;
static HANDLE notify_file;
static bool notify_failed;
static HANDLE late_file;
static pthread_mutex_t held = PTHREAD_MUTEX_INITIALIZER;
static uint32_t main_thread;
static volatile uint32_t worker_threads[WORKERS];
static volatile uint32_t main_count;
static volatile uint32_t worker_counts[WORKERS];
static volatile bool worker_holding;
static volatile bool in_stdio;
static volatile uint32_t lines_written;
static volatile uint32_t last_line_time;
static EX_TITLE_TERMINATE_REGISTRATION registration;

static uint32_t time_base(void)
{
	uint32_t ticks;
	__asm__ volatile("mftb %0" : "=r"(ticks));
	return ticks;
}

static uint32_t ticks_to_us(uint32_t ticks)
{
	return (uint32_t)((uint64_t)ticks * 1000 / TICKS_PER_MS);
}

// The calling thread's processor control region, which r13 points at (src/thread.c).
static volatile const uint8_t *processor_control_region(void)
{
	uint32_t region;
	__asm__ volatile("mr %0, 13" : "=r"(region));
	return (volatile const uint8_t *)(uintptr_t)region;
}

static uint32_t pcr_word(uint32_t offset)
{
	return *(volatile const uint32_t *)(processor_control_region() + offset);
}

// Counts while it waits, yielding so a thread sharing the hardware thread still runs.
static void count_for(uint32_t ms, volatile uint32_t *count)
{
	const uint32_t start = time_base();
	while (time_base() - start < ms * TICKS_PER_MS) {
		*count = *count + 1;
		(void)NtYieldExecution();
	}
}

static NTSTATUS create_file(HANDLE *handle, const char *path)
{
	const ANSI_STRING name = {
		.length = (uint16_t)strlen(path),
		.maximum_length = (uint16_t)(strlen(path) + 1),
		.pointer = path,
	};
	// "game:" is a DOS device; a "\Device" path stands alone (src/log.c).
	const OBJECT_ATTRIBUTES attributes = { .root_directory = path[0] == '\\' ? 0 : OB_DOS_DEVICES, .name = &name };
	IO_STATUS_BLOCK io_status;
	return NtCreateFile(handle, GENERIC_WRITE | SYNCHRONIZE, &attributes, &io_status, NULL, FILE_ATTRIBUTE_NORMAL,
		FILE_SHARE_READ, FILE_OVERWRITE_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
}

static uint32_t put_number(char *text, uint32_t value, bool hex)
{
	char digits[10];
	uint32_t count = 0;
	const uint32_t base = hex ? 16 : 10;
	do {
		digits[count++] = "0123456789ABCDEF"[value % base];
		value /= base;
	} while (value || (hex && count < 8));
	uint32_t length = 0;
	if (hex) {
		text[length++] = '0';
		text[length++] = 'x';
	}
	while (count)
		text[length++] = digits[--count];
	return length;
}

static void note(const char *format, ...);

static HANDLE open_late(void)
{
	for (uint32_t i = 0; i < LATE_PATHS; i++) {
		HANDLE handle;
		if (NT_SUCCESS(create_file(&handle, late_paths[i])))
			return handle;
	}
	return 0;
}

static void write_note(const char *line, uint32_t length)
{
	IO_STATUS_BLOCK io_status;
	if (!notify_failed) {
		const NTSTATUS status = NtWriteFile(notify_file, 0, NULL, NULL, &io_status, line, length, NULL);
		if (NT_SUCCESS(status))
			return;
		notify_failed = true;
		late_file = open_late();
		note(
			"notify: NtWriteFile to %s failed: %x; it and the lines after it come here", notify_path, (uint32_t)status);
	}
	if (late_file)
		(void)NtWriteFile(late_file, 0, NULL, NULL, &io_status, line, length, NULL);
}

// One line of the routine's: %u, %x as 0x%08X and %s, formatted by hand, as newlib's per-thread state may not be set
// up on the routine's thread.
static void note(const char *format, ...)
{
	char line[NOTE_BYTES];
	uint32_t length = 0;
	va_list args;
	va_start(args, format);
	for (const char *c = format; *c && length < NOTE_BYTES - NOTE_SPARE_BYTES; c++) {
		if (*c != '%' || !c[1]) {
			line[length++] = *c;
			continue;
		}
		c++;
		if (*c != 's') {
			length += put_number(line + length, va_arg(args, uint32_t), *c == 'x');
			continue;
		}
		for (const char *s = va_arg(args, const char *); *s && length < NOTE_BYTES - NOTE_SPARE_BYTES; s++)
			line[length++] = *s;
	}
	va_end(args);
	line[length++] = '\n';
	write_note(line, length);
}

static void check_threads(void)
{
	const uint32_t main_before = main_count;
	const uint32_t first_before = worker_counts[0];
	const uint32_t second_before = worker_counts[1];
	const uint32_t start = time_base();
	while (time_base() - start < TICKS_PER_MS)
		(void)NtYieldExecution();
	note("notify: in 1 ms the main thread counted %u, worker 1 %u (holding the mutex: %u), worker 2 %u",
		main_count - main_before, worker_counts[0] - first_before, (uint32_t)worker_holding,
		worker_counts[1] - second_before);
}

static void try_mutex(void)
{
	const uint32_t start = time_base();
	uint32_t tries = 0;
	while (time_base() - start < TRY_FOR_MS * TICKS_PER_MS) {
		tries++;
		if (pthread_mutex_trylock(&held) == 0) {
			(void)pthread_mutex_unlock(&held);
			note("notify: took the worker's mutex at try %u, %u us in", tries, ticks_to_us(time_base() - start));
			return;
		}
		(void)NtYieldExecution();
	}
	note("notify: the worker's mutex was held through %u tries over %u ms", tries, (uint32_t)TRY_FOR_MS);
}

// fflush reaches newlib's state for the calling thread through its TLS, which only the title's own threads are known to
// hold, and the stream's lock, which it would wait for without end.
static void try_flush(uint32_t thread)
{
	note("notify: %u bytes wait in stdio's buffer", (uint32_t)(out->_p - out->_bf._base));
	if (thread != main_thread && thread != worker_threads[0] && thread != worker_threads[1]) {
		note("notify: fflush not tried: the title didn't make this thread, so its TLS may not be newlib's");
		return;
	}
	if (thread == main_thread && in_stdio) {
		note("notify: fflush not tried: this is the main thread, stopped inside stdio");
		return;
	}
	const uint32_t start = time_base();
	uint32_t tries = 1;
	while (!__retarget_lock_try_acquire_recursive(out->_lock)) {
		if (time_base() - start >= TRY_FOR_MS * TICKS_PER_MS) {
			note("notify: fflush not tried: the stream's lock was held through %u tries over %u ms", tries,
				(uint32_t)TRY_FOR_MS);
			return;
		}
		tries++;
		(void)NtYieldExecution();
	}
	const uint32_t written = lines_written;
	const uint32_t flush_start = time_base();
	const int result = fflush(out);
	const uint32_t flush_ticks = time_base() - flush_start;
	__retarget_lock_release_recursive(out->_lock);
	note("notify: fflush returned %u in %u us, the stream's lock taken at try %u; the main thread had written %u lines",
		(uint32_t)result, ticks_to_us(flush_ticks), tries, written);
}

// A line a millisecond, each with how far the title's threads have counted, until HEARTBEAT_MS from start.
static void heartbeat(uint32_t start)
{
	for (;;) {
		const uint32_t elapsed = time_base() - start;
		if (elapsed >= HEARTBEAT_MS * TICKS_PER_MS)
			break;
		note("notify: %u us in: main %u, worker 1 %u, worker 2 %u, lines %u", ticks_to_us(elapsed), main_count,
			worker_counts[0], worker_counts[1], lines_written);
		const uint32_t next = (elapsed / TICKS_PER_MS + 1) * TICKS_PER_MS;
		while (time_base() - start < next)
			(void)NtYieldExecution();
	}
	note("notify: returning, %u us after the routine started", ticks_to_us(time_base() - start));
}

// The kernel's arguments, if it passes any, land in these.
static void on_terminate(uint32_t argument_0, uint32_t argument_1)
{
	const uint32_t start = time_base();
	note("notify: entered");
	uint32_t stack;
	__asm__ volatile("mr %0, 1" : "=r"(stack));
	const uint32_t thread = pcr_word(PCR_CURRENT_THREAD);
	note("notify: thread object %x, hardware thread %u, stack %x, TLS %x, arguments %x %x", thread,
		(uint32_t)processor_control_region()[PCR_CURRENT_PROCESSOR], stack, pcr_word(PCR_TLS), argument_0, argument_1);
	note("notify: the main thread's object is %x, the workers' %x and %x", main_thread, worker_threads[0],
		worker_threads[1]);
	note("notify: time base %x at entry, %u us after the main thread's last line, %u lines written", start,
		ticks_to_us(start - last_line_time), lines_written);
	check_threads();
	try_mutex();
	try_flush(thread);
	heartbeat(start);
	// Left in place, the registration froze the console as the next title started (docs/HARDWARE.md).
	ExRegisterTitleTerminateNotification(&registration, 0);
	note("notify: unregistered itself");
}

static uint32_t hold_mutex(void *context)
{
	(void)context;
	worker_threads[0] = pcr_word(PCR_CURRENT_THREAD);
	for (;;) {
		(void)pthread_mutex_lock(&held);
		worker_holding = true;
		count_for(MUTEX_HELD_MS, &worker_counts[0]);
		worker_holding = false;
		(void)pthread_mutex_unlock(&held);
		count_for(MUTEX_FREE_MS, &worker_counts[0]);
	}
	// Never reached: the title is ended from outside.
	return 0;
}

static uint32_t count(void *context)
{
	(void)context;
	worker_threads[1] = pcr_word(PCR_CURRENT_THREAD);
	for (;;)
		count_for(LINE_SPACING_MS, &worker_counts[1]);
	return 0;
}

static int start_workers(void)
{
	const XsdkThreadEntry entries[WORKERS] = { hold_mutex, count };
	for (uint32_t i = 0; i < WORKERS; i++) {
		HANDLE thread;
		const NTSTATUS status = xsdk_thread_create_on(&thread, 0, entries[i], NULL, worker_hardware_threads[i]);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write(
				"terminate probe: FAILED, worker %u didn't start: 0x%08X", (unsigned)i + 1, (unsigned)status);
			return 1;
		}
		(void)NtClose(thread);
	}
	// Until both have started, so the routine finds the mutex made and their objects recorded.
	while (!worker_threads[0] || !worker_threads[1])
		(void)xsdk_thread_sleep(1);
	return 0;
}

static int run_case(void)
{
	main_thread = pcr_word(PCR_CURRENT_THREAD);
	xsdk_log_write("terminate probe: run: main thread object %08X on hardware thread %u, TLS %08X, time base %08X",
		(unsigned)main_thread, (unsigned)xsdk_thread_hardware_thread(), (unsigned)pcr_word(PCR_TLS),
		(unsigned)time_base());
	NTSTATUS status = create_file(&notify_file, notify_path);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("terminate probe: FAILED, NtCreateFile(%s) returned 0x%08X", notify_path, (unsigned)status);
		return 1;
	}
	note("main: run started: main thread object %x on hardware thread %u, time base %x", main_thread,
		xsdk_thread_hardware_thread(), time_base());
	out = fopen(out_path, "w");
	if (!out) {
		xsdk_log_write("terminate probe: FAILED, fopen(%s) failed: errno %d", out_path, errno);
		return 1;
	}
	if (start_workers())
		return 1;
	xsdk_log_write("terminate probe: workers %08X on hardware thread %u, holding the mutex %u ms in %u, and %08X on %u",
		(unsigned)worker_threads[0], (unsigned)worker_hardware_threads[0], (unsigned)MUTEX_HELD_MS,
		(unsigned)(MUTEX_HELD_MS + MUTEX_FREE_MS), (unsigned)worker_threads[1], (unsigned)worker_hardware_threads[1]);
	registration.notification_routine = (void (*)(void))on_terminate;
	registration.priority = NOTIFY_PRIORITY;
	ExRegisterTitleTerminateNotification(&registration, 1);
	xsdk_log_write(
		"terminate probe: notification registered at priority %u; a line every %u ms to %s, stdio's "
		"buffer %u bytes, never flushed, until the title is ended from outside",
		(unsigned)NOTIFY_PRIORITY, (unsigned)LINE_SPACING_MS, out_path, (unsigned)BUFSIZ);
	for (uint32_t line = 0;; line++) {
		const uint32_t now = time_base();
		in_stdio = true;
		const int printed = fprintf(out, "line %u time base %08X\n", (unsigned)line, (unsigned)now);
		in_stdio = false;
		if (printed < 0)
			xsdk_log_write("terminate probe: fprintf of line %u failed: errno %d", (unsigned)line, errno);
		last_line_time = now;
		lines_written = line + 1;
		if (line % LINES_PER_LOG == 0)
			xsdk_log_write("terminate probe: line %u, time base %08X", (unsigned)line, (unsigned)now);
		count_for(LINE_SPACING_MS, &main_count);
	}
}

// Logs every line of a file the routine wrote.
static void log_file(const char *path)
{
	FILE *const file = fopen(path, "r");
	if (!file) {
		xsdk_log_write("terminate probe: read: %s: not there (errno %d)", path, errno);
		return;
	}
	char line[READ_LINE_BYTES];
	uint32_t lines = 0;
	while (fgets(line, sizeof line, file)) {
		line[strcspn(line, "\n")] = '\0';
		xsdk_log_write("terminate probe: read: %s", line);
		lines++;
	}
	(void)fclose(file);
	xsdk_log_write("terminate probe: read: %s: %u lines", path, (unsigned)lines);
}

// Whether terminate_out.txt numbers its lines 0, 1, 2 and on with none missing, and its last few.
static void check_out(void)
{
	FILE *const file = fopen(out_path, "r");
	if (!file) {
		xsdk_log_write("terminate probe: read: %s: not there (errno %d)", out_path, errno);
		return;
	}
	char line[READ_LINE_BYTES];
	char last[LAST_LINES][READ_LINE_BYTES] = { "" };
	uint32_t lines = 0;
	bool in_order = true;
	while (fgets(line, sizeof line, file)) {
		line[strcspn(line, "\n")] = '\0';
		unsigned number = 0;
		if (in_order && (sscanf(line, "line %u", &number) != 1 || number != lines)) {
			xsdk_log_write("terminate probe: read: %s: line %u reads \"%s\"", out_path, (unsigned)lines, line);
			in_order = false;
		}
		strcpy(last[lines % LAST_LINES], line);
		lines++;
	}
	(void)fclose(file);
	xsdk_log_write("terminate probe: read: %s: %u lines, %s", out_path, (unsigned)lines,
		in_order ? "numbered 0 on in order" : "NOT in order");
	for (uint32_t i = lines > LAST_LINES ? lines - LAST_LINES : 0; i < lines; i++)
		xsdk_log_write("terminate probe: read: %s: \"%s\"", out_path, last[i % LAST_LINES]);
}

static int read_case(void)
{
	log_file(notify_path);
	for (uint32_t i = 0; i < LATE_PATHS; i++)
		log_file(late_paths[i]);
	check_out();
	return 0;
}

typedef struct {
	const char *name;
	const char *what;
	int (*run)(void);
} ProbeCase;

static const ProbeCase cases[] = {
	{ "read", "logs what the last run's routine wrote, and how much of terminate_out.txt survived", read_case },
	{ "run", "writes through stdio until ended from outside, the notification's routine recording", run_case },
};

enum { CASES = sizeof cases / sizeof cases[0] };

static const ProbeCase *start_case(void)
{
	FILE *const file = fopen(case_path, "r");
	char name[NAME_BYTES] = "";
	const bool read = file && fscanf(file, "%15s", name) == 1;
	if (file)
		(void)fclose(file);
	for (uint32_t i = 0; read && i < CASES; i++) {
		if (!strcmp(name, cases[i].name))
			return &cases[i];
	}
	xsdk_log_write("terminate probe: %s names no case, so read; the cases:", case_path);
	for (uint32_t i = 0; i < CASES; i++)
		xsdk_log_write("terminate probe:   %s: %s", cases[i].name, cases[i].what);
	return &cases[0];
}

int main(void)
{
	const ProbeCase *const probe_case = start_case();
	xsdk_log_write("terminate probe: case %s: %s", probe_case->name, probe_case->what);
	return probe_case->run();
}
