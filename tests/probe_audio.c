#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "probe.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// Milestone 58 step 2 (docs/scope/58-audio.md): the kernel's render driver and how a feeder wakes, on the console.
// probe_audio_start.txt beside the .xex names the cases to run, in order. Without it, every case but callback_wake,
// which signals from inside the kernel's callback and could stop the console: it runs only when named, and warns before
// it starts, for net_log.txt to carry (docs/HARDWARE.md, "The log over the network, milestone 53").

enum {
	// The time base's rate (docs/HARDWARE.md, "The time base runs at 49.875 MHz").
	TICKS_PER_MS = 49875,
	// 10 us between two reads of the time base counts as time the thread didn't run.
	OFF_TICKS = TICKS_PER_MS / 100,
	// 6 channels of 256 big-endian floats (docs/HARDWARE.md, "Runtime, milestone 2").
	FRAME_BYTES = 6 * 256 * 4,
	// The processor control region's thread, hardware thread and DPC flag (xenia:src/xenia/kernel/xthread.h:73-86).
	PCR_CURRENT_THREAD = 0x100,
	PCR_CURRENT_PROCESSOR = 0x10C,
	PCR_DPC_ACTIVE = 0x150,
	// xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_table.inc
	ORDINAL_KE_SET_EVENT = 0x09D,
	ORDINAL_GET_SPEAKER_CONFIG = 0x1FF,
	WHERE_MS = 200,
	// Xenia runs 64 callbacks as a client registers (xenia:src/xenia/apu/audio_system.cc:179).
	EARLY_MS = 50,
	RUN_MS = 1000,
	SECOND_CLIENT_MS = 500,
	WAKE_CHECK_MS = 100,
	// A callback can still be running as unregistering returns (xenia:src/xenia/apu/audio_system.cc:210-227).
	SETTLE_MS = 20,
	FIRST_CALLBACKS = 8,
	// More callback times than a run's 188 a second.
	TAKEN_RING = 256,
	BUSY_HARDWARE_THREAD = 1,
	STACK_BYTES = 64 * 1024,
	TIMER_PERIOD_MS = 5,
	FEEDER_WAIT_MS = 100,
	THREAD_WAIT_MS = 5000,
	CASE_NAME_BYTES = 32,
	MAX_RUN = 16,
	LINE_BYTES = 160,
};

// 100 ns units.
static const int64_t MS_100NS = 10000;
static const uint32_t UNWRITTEN = 0xA5A5A5A5;
// The default and the levels milestone 55 found (docs/GUIDE.md, "Threads and where they run").
static const int32_t priorities[] = { 0, 1, 2, 16 };

enum { PRIORITY_COUNT = sizeof priorities / sizeof priorities[0] };

typedef enum { WAKE_SLEEP, WAKE_TIMER, WAKE_EVENT } WakeMode;

static const char *const wake_names[] = { "a 1 ms sleep", "a 5 ms periodic timer", "the callback's event" };

// KeSetEvent(event object, increment, wait) (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc:406-420).
typedef int32_t (*KeSetEventCall)(uint32_t event, int32_t increment, uint32_t wait);
// XAudioGetSpeakerConfig(&config) (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_audio.cc:22-26).
typedef uint32_t (*GetSpeakerConfigCall)(uint32_t *config);

// What a client's callbacks saw. Only its callback writes it, except the driver, which registering stores.
typedef struct {
	uint32_t driver;
	uint32_t register_tick;
	uint32_t callbacks;
	uint32_t before_return;
	uint32_t early;
	uint32_t first_ticks[FIRST_CALLBACKS];
	uint32_t taken_tick[TAKEN_RING];
	uint32_t gap_min;
	uint32_t gap_max;
	uint32_t hardware_threads;
	uint32_t thread_object;
	uint32_t other_threads;
	uint32_t dpc_set;
	uint32_t dpc_value;
	// callback_wake's event, 0 for none.
	HANDLE event;
	uint32_t event_object;
	uint32_t ke_sets;
	int32_t ke_result;
	uint32_t nt_sets;
	NTSTATUS nt_status;
} Client;

typedef struct {
	volatile uint32_t ready;
	uint32_t passes;
	uint32_t offs;
	uint32_t off_ticks;
	uint32_t longest_off;
} Spinner;

typedef struct {
	Client *client;
	WakeMode mode;
	int32_t priority;
	volatile uint32_t ready;
	volatile uint32_t stop;
	uint32_t wakes;
	uint32_t frames;
	uint32_t latest_frame;
	uint32_t longest_gap;
	NTSTATUS first_failure;
} Feeder;

typedef struct {
	const char *name;
	// What may go wrong, logged before the case runs; NULL if nothing, and then it runs by default.
	const char *risk;
	void (*run)(void);
} ProbeCase;

// What callbacks and threads touch is static: a callback can outlast unregistering, and a thread its wait.
static float *silence;
static Client clients[2];
static KeSetEventCall ke_set_event;
static volatile uint32_t go;

static uint32_t to_us(uint32_t ticks)
{
	return (uint32_t)((uint64_t)ticks * 1000 / TICKS_PER_MS);
}

static uint32_t pcr_word(uint32_t offset)
{
	uint32_t region;
	__asm__ volatile("mr %0, 13" : "=r"(region));
	return *(volatile const uint32_t *)(uintptr_t)(region + offset);
}

static uint32_t pcr_byte(uint32_t offset)
{
	uint32_t region;
	__asm__ volatile("mr %0, 13" : "=r"(region));
	return *(volatile const uint8_t *)(uintptr_t)(region + offset);
}

static bool resolve(uint32_t ordinal, void **slot)
{
	HANDLE kernel = 0;
	const uint32_t error = XexGetModuleHandle("xboxkrnl.exe", &kernel);
	if (error != 0) {
		xsdk_log_write("audio: XexGetModuleHandle(xboxkrnl.exe) failed: %08X", (unsigned)error);
		return false;
	}
	if (XexGetProcedureAddress(kernel, ordinal, slot) < 0 || !*slot) {
		xsdk_log_write("audio: xboxkrnl.exe has no ordinal 0x%03X", (unsigned)ordinal);
		return false;
	}
	return true;
}

// The callback's half of callback_wake: KeSetEvent on the event's object. The callback runs in a kernel thread of its
// own, outside the title's handles: NtSetEvent on the title's handle there returned 0xC0000008 on the console, while
// the object is the same one in any thread.
static void wake_feeder(Client *client, uint32_t dpc)
{
	(void)dpc;
	client->ke_result = ke_set_event(client->event_object, 0, 0);
	client->ke_sets++;
}

// Runs on the kernel's audio thread for each frame the driver wants; integers only (docs/GUIDE.md, "Input and audio
// through the kernel").
static void callback(const uint32_t *context)
{
	Client *const client = (Client *)(uintptr_t)*context;
	const uint32_t now = probe_time_base();
	const uint32_t count = __atomic_load_n(&client->callbacks, __ATOMIC_RELAXED);
	const uint32_t early_ticks = EARLY_MS * TICKS_PER_MS;
	const uint32_t driver = __atomic_load_n(&client->driver, __ATOMIC_ACQUIRE);
	client->before_return += !driver;
	if (now - client->register_tick < early_ticks)
		client->early++;
	if (count < FIRST_CALLBACKS)
		client->first_ticks[count] = now - client->register_tick;
	const uint32_t previous = client->taken_tick[(count - 1) % TAKEN_RING];
	if (count && previous - client->register_tick >= early_ticks) {
		const uint32_t gap = now - previous;
		client->gap_min = gap < client->gap_min ? gap : client->gap_min;
		client->gap_max = gap > client->gap_max ? gap : client->gap_max;
	}
	client->taken_tick[count % TAKEN_RING] = now;

	client->hardware_threads |= 1u << (pcr_byte(PCR_CURRENT_PROCESSOR) & 31);
	const uint32_t thread = pcr_word(PCR_CURRENT_THREAD);
	if (count == 0)
		client->thread_object = thread;
	client->other_threads += thread != client->thread_object;
	const uint32_t dpc = pcr_word(PCR_DPC_ACTIVE);
	if (dpc) {
		client->dpc_set++;
		client->dpc_value = dpc;
	}
	__atomic_store_n(&client->callbacks, count + 1, __ATOMIC_RELEASE);

	if (client->event)
		wake_feeder(client, dpc);
	if (driver)
		(void)XAudioSubmitRenderDriverFrame(driver, silence);
}

// Registers a client whose callback hands over silence; with an event, the callback sets it each time.
static bool open_client(Client *client, HANDLE event, uint32_t event_object)
{
	memset(client, 0, sizeof *client);
	client->gap_min = UINT32_MAX;
	client->event = event;
	client->event_object = event_object;
	const XAUDIO_RENDER_DRIVER_CLIENT registration = { .callback = callback, .context = (uint32_t)(uintptr_t)client };
	uint32_t driver = 0;
	client->register_tick = probe_time_base();
	const uint32_t result = XAudioRegisterRenderDriverClient(&registration, &driver);
	const uint32_t took = probe_time_base() - client->register_tick;
	xsdk_log_write("audio: XAudioRegisterRenderDriverClient: %08X, driver %08X, in %u us, %u callbacks by then",
		(unsigned)result, (unsigned)driver, (unsigned)to_us(took),
		(unsigned)__atomic_load_n(&client->callbacks, __ATOMIC_ACQUIRE));
	if (result != ERROR_SUCCESS)
		return false;
	__atomic_store_n(&client->driver, driver, __ATOMIC_RELEASE);
	return true;
}

static void close_client(Client *client)
{
	const uint32_t driver = __atomic_exchange_n(&client->driver, 0, __ATOMIC_ACQ_REL);
	xsdk_log_write("audio: XAudioUnregisterRenderDriverClient(%08X): %08X", (unsigned)driver,
		(unsigned)XAudioUnregisterRenderDriverClient(driver));
	(void)xsdk_thread_sleep(SETTLE_MS);
}

static void wait_ready(volatile const uint32_t *ready)
{
	while (!__atomic_load_n(ready, __ATOMIC_ACQUIRE))
		(void)xsdk_thread_sleep(1);
}

static void wait_go(void)
{
	while (!__atomic_load_n(&go, __ATOMIC_ACQUIRE))
		(void)xsdk_thread_sleep(1);
}

static HANDLE start(XsdkThreadEntry entry, void *context)
{
	HANDLE thread = 0;
	const NTSTATUS status = xsdk_thread_create_on(&thread, STACK_BYTES, entry, context, BUSY_HARDWARE_THREAD);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("audio: xsdk_thread_create_on failed: 0x%08X", (unsigned)status);
		return 0;
	}
	return thread;
}

static void join(HANDLE thread)
{
	if (!thread)
		return;
	const int64_t timeout = -THREAD_WAIT_MS * MS_100NS;
	const NTSTATUS status = NtWaitForSingleObjectEx(thread, USER_MODE, 0, &timeout);
	if (status != STATUS_SUCCESS)
		xsdk_log_write("audio: a thread didn't end in %d ms: 0x%08X", THREAD_WAIT_MS, (unsigned)status);
	(void)NtClose(thread);
}

// Busy for RUN_MS, noting each time it went OFF_TICKS or more without running.
static uint32_t spin(void *context)
{
	Spinner *const spinner = context;
	__atomic_store_n(&spinner->ready, 1, __ATOMIC_RELEASE);
	wait_go();
	const uint32_t start_tick = probe_time_base();
	uint32_t previous = start_tick;
	while (previous - start_tick < RUN_MS * TICKS_PER_MS) {
		const uint32_t now = probe_time_base();
		const uint32_t gap = now - previous;
		if (gap >= OFF_TICKS) {
			spinner->offs++;
			spinner->off_ticks += gap;
			spinner->longest_off = gap > spinner->longest_off ? gap : spinner->longest_off;
		}
		previous = now;
		spinner->passes++;
	}
	return 0;
}

static NTSTATUS start_timer(HANDLE *timer)
{
	NTSTATUS status = NtCreateTimer(timer, NULL, SYNCHRONIZATION_TIMER);
	if (!NT_SUCCESS(status))
		return status;
	const int64_t due = -TIMER_PERIOD_MS * MS_100NS;
	status = NtSetTimerEx(*timer, &due, NULL, 1, NULL, 0, TIMER_PERIOD_MS, 0);
	if (!NT_SUCCESS(status))
		(void)NtClose(*timer);
	return status;
}

// Wakes by its mode until stopped, and notes how long after the kernel took each frame it saw it.
static uint32_t feed(void *context)
{
	Feeder *const feeder = context;
	Client *const client = feeder->client;
	feeder->first_failure = xsdk_thread_set_priority(XSDK_CURRENT_THREAD, feeder->priority, NULL);
	__atomic_store_n(&feeder->ready, 1, __ATOMIC_RELEASE);
	wait_go();
	HANDLE waitable = client->event;
	if (feeder->mode == WAKE_TIMER) {
		const NTSTATUS status = start_timer(&waitable);
		if (!NT_SUCCESS(status)) {
			feeder->first_failure = status;
			return 1;
		}
	}
	const int64_t timeout = -FEEDER_WAIT_MS * MS_100NS;
	uint32_t seen = __atomic_load_n(&client->callbacks, __ATOMIC_ACQUIRE);
	uint32_t previous = probe_time_base();
	while (!__atomic_load_n(&feeder->stop, __ATOMIC_ACQUIRE)) {
		NTSTATUS status;
		if (feeder->mode == WAKE_SLEEP)
			status = xsdk_thread_sleep(1);
		else
			status = NtWaitForSingleObjectEx(waitable, USER_MODE, 0, &timeout);
		const uint32_t now = probe_time_base();
		if (status != STATUS_SUCCESS && feeder->first_failure == STATUS_SUCCESS)
			feeder->first_failure = status;
		feeder->wakes++;
		const uint32_t gap = now - previous;
		feeder->longest_gap = gap > feeder->longest_gap ? gap : feeder->longest_gap;
		previous = now;
		const uint32_t count = __atomic_load_n(&client->callbacks, __ATOMIC_ACQUIRE);
		// shortcut: only the last TAKEN_RING callback times are kept, so a feeder further behind is timed from those.
		if (count - seen > TAKEN_RING)
			seen = count - TAKEN_RING;
		for (; seen != count; seen++) {
			const uint32_t late = now - client->taken_tick[seen % TAKEN_RING];
			feeder->latest_frame = late > feeder->latest_frame ? late : feeder->latest_frame;
			feeder->frames++;
		}
	}
	if (feeder->mode == WAKE_TIMER) {
		uint32_t state = 0;
		(void)NtCancelTimer(waitable, &state);
		(void)NtClose(waitable);
	}
	return 0;
}

// The busy thread on BUSY_HARDWARE_THREAD for RUN_MS, alone or with the feeder beside it.
static bool run_busy(Spinner *busy, Feeder *feeder)
{
	__atomic_store_n(&go, 0, __ATOMIC_RELEASE);
	const HANDLE busy_thread = start(spin, busy);
	const HANDLE feeder_thread = busy_thread && feeder ? start(feed, feeder) : 0;
	const bool started = busy_thread && (!feeder || feeder_thread);
	if (started) {
		wait_ready(&busy->ready);
		if (feeder)
			wait_ready(&feeder->ready);
	}
	__atomic_store_n(&go, 1, __ATOMIC_RELEASE);
	join(busy_thread);
	if (feeder)
		__atomic_store_n(&feeder->stop, 1, __ATOMIC_RELEASE);
	join(feeder_thread);
	return started;
}

static void log_sets(const Client *client, const char *when)
{
	xsdk_log_write(
		"audio: callback_wake: %s, %u callbacks: %u set the event by KeSetEvent, the last returning %d; %u "
		"by NtSetEvent, the last 0x%08X",
		when, (unsigned)__atomic_load_n(&client->callbacks, __ATOMIC_ACQUIRE), (unsigned)client->ke_sets,
		(int)client->ke_result, (unsigned)client->nt_sets, (unsigned)client->nt_status);
}

// A busy thread alone, then beside a feeder that wakes by each mode at each priority.
static void run_feeders(Client *client, WakeMode first, WakeMode last)
{
	static Spinner alone;
	memset(&alone, 0, sizeof alone);
	if (!run_busy(&alone, NULL))
		return;
	xsdk_log_write(
		"audio: feeder: a busy thread alone on hardware thread %d: %u passes in %d ms, off %u times, "
		"%u us at most",
		BUSY_HARDWARE_THREAD, (unsigned)alone.passes, RUN_MS, (unsigned)alone.offs, (unsigned)to_us(alone.longest_off));
	for (WakeMode mode = first; mode <= last; mode++) {
		for (int i = 0; i < PRIORITY_COUNT; i++) {
			static Spinner busy;
			static Feeder feeder;
			memset(&busy, 0, sizeof busy);
			memset(&feeder, 0, sizeof feeder);
			feeder.client = client;
			feeder.mode = mode;
			feeder.priority = priorities[i];
			if (!run_busy(&busy, &feeder))
				continue;
			const double lost = alone.passes ? 100.0 * ((double)alone.passes - busy.passes) / alone.passes : 0.0;
			xsdk_log_write(
				"audio: feeder: %s at %+d: %u wakes, %u frames seen, the latest %u us after the kernel "
				"took it, %u us between wakes at most, first failed call 0x%08X; the busy thread lost "
				"%.1f%% of its passes, off %u us at most",
				wake_names[mode], (int)priorities[i], (unsigned)feeder.wakes, (unsigned)feeder.frames,
				(unsigned)to_us(feeder.latest_frame), (unsigned)to_us(feeder.longest_gap),
				(unsigned)feeder.first_failure, lost, (unsigned)to_us(busy.longest_off));
		}
	}
}

static void run_where(void)
{
	Client *const client = &clients[0];
	if (!open_client(client, 0, 0))
		return;
	(void)xsdk_thread_sleep(WHERE_MS);
	xsdk_log_write(
		"audio: where: %u callbacks, on hardware threads %02X; the first on thread object %08X, %u on "
		"another; the DPC flag set in %u, last %08X. The main thread: hardware thread %u, thread object "
		"%08X, DPC flag %08X",
		(unsigned)__atomic_load_n(&client->callbacks, __ATOMIC_ACQUIRE), (unsigned)client->hardware_threads,
		(unsigned)client->thread_object, (unsigned)client->other_threads, (unsigned)client->dpc_set,
		(unsigned)client->dpc_value, (unsigned)pcr_byte(PCR_CURRENT_PROCESSOR), (unsigned)pcr_word(PCR_CURRENT_THREAD),
		(unsigned)pcr_word(PCR_DPC_ACTIVE));
	close_client(client);
}

static void run_ahead(void)
{
	Client *const client = &clients[0];
	if (!open_client(client, 0, 0))
		return;
	(void)xsdk_thread_sleep(EARLY_MS + RUN_MS);
	const uint32_t elapsed = probe_time_base() - client->register_tick;
	const uint32_t callbacks = __atomic_load_n(&client->callbacks, __ATOMIC_ACQUIRE);
	char first[LINE_BYTES] = "";
	for (uint32_t i = 0; i < FIRST_CALLBACKS && i < callbacks; i++)
		snprintf(first + strlen(first), sizeof first - strlen(first), " %u", (unsigned)to_us(client->first_ticks[i]));
	xsdk_log_write(
		"audio: ahead: %u callbacks in the first %d ms, %u before registering returned; the first at%s us; "
		"then %u in %u ms, %u to %u us apart",
		(unsigned)client->early, EARLY_MS, (unsigned)client->before_return, first,
		(unsigned)(callbacks - client->early), (unsigned)(to_us(elapsed) / 1000 - EARLY_MS),
		(unsigned)to_us(client->gap_min), (unsigned)to_us(client->gap_max));
	close_client(client);
}

static void run_second_client(void)
{
	if (!open_client(&clients[0], 0, 0))
		return;
	const bool second = open_client(&clients[1], 0, 0);
	const uint32_t first_before = __atomic_load_n(&clients[0].callbacks, __ATOMIC_ACQUIRE);
	(void)xsdk_thread_sleep(SECOND_CLIENT_MS);
	xsdk_log_write(
		"audio: second_client: registered %s; in the next %d ms the first was called %u times, the second %u",
		second ? "yes" : "no", SECOND_CLIENT_MS,
		(unsigned)(__atomic_load_n(&clients[0].callbacks, __ATOMIC_ACQUIRE) - first_before),
		(unsigned)__atomic_load_n(&clients[1].callbacks, __ATOMIC_ACQUIRE));
	if (second)
		close_client(&clients[1]);
	close_client(&clients[0]);
}

static void run_time_slice(void)
{
	static Spinner spinners[2];
	memset(spinners, 0, sizeof spinners);
	__atomic_store_n(&go, 0, __ATOMIC_RELEASE);
	const HANDLE first = start(spin, &spinners[0]);
	const HANDLE second = first ? start(spin, &spinners[1]) : 0;
	if (second) {
		wait_ready(&spinners[0].ready);
		wait_ready(&spinners[1].ready);
	}
	__atomic_store_n(&go, 1, __ATOMIC_RELEASE);
	join(first);
	join(second);
	if (!second)
		return;
	const uint32_t total = spinners[0].passes + spinners[1].passes;
	for (int i = 0; i < 2; i++) {
		const Spinner *const spinner = &spinners[i];
		xsdk_log_write(
			"audio: time_slice: busy thread %d of 2 at the default priority on hardware thread %d: %u%% of "
			"the passes, off %u times, %u us in all, %u us at most",
			i + 1, BUSY_HARDWARE_THREAD, total ? (unsigned)((uint64_t)spinner->passes * 100 / total) : 0,
			(unsigned)spinner->offs, (unsigned)to_us(spinner->off_ticks), (unsigned)to_us(spinner->longest_off));
	}
}

static void run_feeder(void)
{
	Client *const client = &clients[0];
	if (!open_client(client, 0, 0))
		return;
	run_feeders(client, WAKE_SLEEP, WAKE_TIMER);
	close_client(client);
}

static void run_speakers(void)
{
	GetSpeakerConfigCall get_speaker_config;
	if (!resolve(ORDINAL_GET_SPEAKER_CONFIG, (void **)&get_speaker_config))
		return;
	uint32_t config = UNWRITTEN;
	const uint32_t result = get_speaker_config(&config);
	xsdk_log_write("audio: speakers: XAudioGetSpeakerConfig returned %08X, the word %08X (%08X if unwritten)",
		(unsigned)result, (unsigned)config, (unsigned)UNWRITTEN);
}

static void run_callback_wake(void)
{
	if (!resolve(ORDINAL_KE_SET_EVENT, (void **)&ke_set_event))
		return;
	HANDLE event = 0;
	const NTSTATUS created = NtCreateEvent(&event, NULL, SYNCHRONIZATION_EVENT, 0);
	if (!NT_SUCCESS(created)) {
		xsdk_log_write("audio: NtCreateEvent failed: 0x%08X", (unsigned)created);
		return;
	}
	uint32_t object = 0;
	const NTSTATUS referenced = ObReferenceObjectByHandle(event, 0, &object);
	xsdk_log_write("audio: callback_wake: ObReferenceObjectByHandle(event): 0x%08X, object %08X", (unsigned)referenced,
		(unsigned)object);
	if (!NT_SUCCESS(referenced) || !object) {
		(void)NtClose(event);
		return;
	}
	xsdk_log_write(
		"audio: callback_wake: registering a client whose callback sets an event, by KeSetEvent where the "
		"DPC flag is set and NtSetEvent where it isn't; if the kernel won't take that, the console may "
		"stop here");
	Client *const client = &clients[0];
	if (open_client(client, event, object)) {
		(void)xsdk_thread_sleep(WAKE_CHECK_MS);
		log_sets(client, "registered 100 ms");
		run_feeders(client, WAKE_EVENT, WAKE_EVENT);
		log_sets(client, "after the feeders");
		close_client(client);
	}
	(void)ObDereferenceObject(object);
	(void)NtClose(event);
}

static const ProbeCase cases[] = {
	{ "where", NULL, run_where },
	{ "ahead", NULL, run_ahead },
	{ "second_client", NULL, run_second_client },
	{ "time_slice", NULL, run_time_slice },
	{ "feeder", NULL, run_feeder },
	{ "speakers", NULL, run_speakers },
	{ "callback_wake", "the kernel may stop the console if its audio callback sets an event", run_callback_wake },
};

enum { CASE_COUNT = sizeof cases / sizeof cases[0] };

// The cases probe_audio_start.txt names, in order; without it, or naming none, every case without a risk.
static uint32_t read_start(const ProbeCase *chosen[MAX_RUN])
{
	uint32_t count = 0;
	FILE *const file = fopen("probe_audio_start.txt", "r");
	char word[CASE_NAME_BYTES];
	while (file && count < MAX_RUN && fscanf(file, "%31s", word) == 1) {
		uint32_t i = 0;
		while (i < CASE_COUNT && strcmp(word, cases[i].name) != 0)
			i++;
		if (i < CASE_COUNT)
			chosen[count++] = &cases[i];
		else
			xsdk_log_write("audio: probe_audio_start.txt names no case '%s'", word);
	}
	if (file)
		(void)fclose(file);
	if (count == 0) {
		for (uint32_t i = 0; i < CASE_COUNT; i++) {
			if (!cases[i].risk)
				chosen[count++] = &cases[i];
		}
	}
	return count;
}

int main(void)
{
	silence = MmAllocatePhysicalMemoryEx(0, FRAME_BYTES, PAGE_READWRITE, 0, UINT32_MAX, 0);
	if (!silence) {
		xsdk_log_write("audio: MmAllocatePhysicalMemoryEx failed");
		return 1;
	}
	memset(silence, 0, FRAME_BYTES);
	const ProbeCase *chosen[MAX_RUN];
	const uint32_t count = read_start(chosen);
	FILE *const net_log = fopen("net_log.txt", "r");
	if (net_log)
		(void)fclose(net_log);
	char risks[LINE_BYTES] = "";
	for (uint32_t i = 0; i < count; i++) {
		if (chosen[i]->risk)
			snprintf(risks + strlen(risks), sizeof risks - strlen(risks), " %s: %s.", chosen[i]->name, chosen[i]->risk);
	}
	xsdk_log_write("audio probe: %u cases, net_log.txt %s.%s", (unsigned)count, net_log ? "beside" : "not beside",
		risks[0] ? risks : " None should hang.");
	for (uint32_t i = 0; i < count; i++) {
		const uint32_t start_tick = probe_time_base();
		xsdk_log_write("audio probe: case %s", chosen[i]->name);
		chosen[i]->run();
		xsdk_log_write("audio probe: case %s done in %u ms", chosen[i]->name,
			(unsigned)(to_us(probe_time_base() - start_tick) / 1000));
	}
	xsdk_log_write("audio probe: done");
	return 0;
}
