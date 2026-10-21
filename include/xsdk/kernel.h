#ifndef XSDK_KERNEL_H
#define XSDK_KERNEL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Types, constants and imports as xenia:src/xenia/xbox.h and the shims in src/xenia/kernel/ define them.

typedef int32_t NTSTATUS;
typedef uint32_t HANDLE;

typedef struct {
	uint16_t length;
	uint16_t maximum_length;
	const char *pointer;
} ANSI_STRING;

typedef struct {
	HANDLE root_directory;
	const ANSI_STRING *name;
	uint32_t attributes;
} OBJECT_ATTRIBUTES;

typedef struct {
	NTSTATUS status;
	uint32_t information;
} IO_STATUS_BLOCK;

// The kernel owns the layout (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_rtl.cc:369-376).
typedef struct {
	uint32_t opaque[7];
} RTL_CRITICAL_SECTION;

// xenia:src/xenia/kernel/info/file.h:98-108
typedef struct {
	uint64_t creation_time;
	uint64_t last_access_time;
	uint64_t last_write_time;
	uint64_t change_time;
	uint64_t allocation_size;
	uint64_t end_of_file;
	uint32_t attributes;
	uint32_t pad;
} FILE_NETWORK_OPEN_INFORMATION;

// One NtQueryDirectoryFile entry (xenia:src/xenia/kernel/xfile.h:36-49).
typedef struct {
	uint32_t next_entry_offset;
	uint32_t file_index;
	uint64_t creation_time;
	uint64_t last_access_time;
	uint64_t last_write_time;
	uint64_t change_time;
	uint64_t end_of_file;
	uint64_t allocation_size;
	uint32_t attributes;
	uint32_t file_name_length;
	char file_name[];
} FILE_DIRECTORY_INFORMATION;

// xenia:src/xenia/kernel/info/file.h:68-83
typedef struct {
	uint8_t delete_file;
} FILE_DISPOSITION_INFORMATION;

typedef struct {
	uint64_t end_of_file;
} FILE_END_OF_FILE_INFORMATION;

// NtQueryVolumeInformationFile's size class (xenia:src/xenia/kernel/info/volume.h:39-45).
typedef struct {
	uint64_t total_allocation_units;
	uint64_t available_allocation_units;
	uint32_t sectors_per_allocation_unit;
	uint32_t bytes_per_sector;
} FILE_FS_SIZE_INFORMATION;

// 16 bytes (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_io_info.cc:170-172), laid out as the original Xbox kernel's.
typedef struct {
	uint8_t replace_if_exists;
	HANDLE root_directory;
	ANSI_STRING name;
} FILE_RENAME_INFORMATION;

// INPUT_GAMEPAD's buttons (xenia:src/xenia/hid/input.h:32-46).
enum {
	INPUT_GAMEPAD_DPAD_UP = 0x0001,
	INPUT_GAMEPAD_DPAD_DOWN = 0x0002,
	INPUT_GAMEPAD_DPAD_LEFT = 0x0004,
	INPUT_GAMEPAD_DPAD_RIGHT = 0x0008,
	INPUT_GAMEPAD_START = 0x0010,
	INPUT_GAMEPAD_BACK = 0x0020,
	INPUT_GAMEPAD_LEFT_THUMB = 0x0040,
	INPUT_GAMEPAD_RIGHT_THUMB = 0x0080,
	INPUT_GAMEPAD_LEFT_SHOULDER = 0x0100,
	INPUT_GAMEPAD_RIGHT_SHOULDER = 0x0200,
	INPUT_GAMEPAD_GUIDE = 0x0400,
	INPUT_GAMEPAD_A = 0x1000,
	INPUT_GAMEPAD_B = 0x2000,
	INPUT_GAMEPAD_X = 0x4000,
	INPUT_GAMEPAD_Y = 0x8000,
};

// xenia:src/xenia/hid/input.h:57-72
typedef struct {
	uint16_t buttons;
	uint8_t left_trigger;
	uint8_t right_trigger;
	int16_t thumb_lx;
	int16_t thumb_ly;
	int16_t thumb_rx;
	int16_t thumb_ry;
} INPUT_GAMEPAD;

typedef struct {
	uint32_t packet_number;
	INPUT_GAMEPAD gamepad;
} INPUT_STATE;

// xenia:src/xenia/hid/input.h:74-77
typedef struct {
	uint16_t left_motor_speed;
	uint16_t right_motor_speed;
} INPUT_VIBRATION;

// xenia:src/xenia/xbox.h:257-269
typedef struct {
	uint32_t display_width;
	uint32_t display_height;
	uint32_t is_interlaced;
	uint32_t is_widescreen;
	uint32_t is_hi_def;
	float refresh_rate;
	uint32_t video_standard;
	uint32_t reserved[5];
} VIDEO_MODE;

// xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc:88-105
typedef struct {
	uint16_t front_buffer_width;
	uint16_t front_buffer_height;
	uint8_t front_buffer_color_format;
	uint8_t front_buffer_pixel_format;
	uint32_t scaler_parameters[14];
	uint16_t display_window_overscan[4];
	uint16_t display_width;
	uint16_t display_height;
	float display_refresh_rate;
	uint32_t display_interlaced;
	uint8_t display_color_format;
	uint16_t actual_display_width;
} DISPLAY_INFO;

// Pages are 4 KB here (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc:451-473, 496).
typedef struct {
	uint32_t available_pages;
	uint32_t total_virtual_memory_bytes;
	uint32_t reserved_virtual_memory_bytes;
	uint32_t physical_pages;
	uint32_t pool_pages;
	uint32_t stack_pages;
	uint32_t image_pages;
	uint32_t heap_pages;
	uint32_t virtual_pages;
	uint32_t page_table_pages;
	uint32_t cache_pages;
} MM_QUERY_STATISTICS_SECTION;

typedef struct {
	uint32_t size;
	uint32_t total_physical_pages;
	uint32_t kernel_pages;
	MM_QUERY_STATISTICS_SECTION title;
	MM_QUERY_STATISTICS_SECTION system;
	uint32_t highest_physical_page;
} MM_QUERY_STATISTICS_RESULT;

// The kernel calls back with a pointer to a word holding context (xenia:src/xenia/apu/audio_system.cc:189-192).
typedef struct {
	void (*callback)(const uint32_t *context);
	uint32_t context;
} XAUDIO_RENDER_DRIVER_CLIENT;

// The kernel links it into its list, so it must outlive the registration. Xenia keeps the priority without using it,
// and its call of the routine, commented out, passes nothing (xenia:src/xenia/xbox.h:300-305,
// xenia:src/xenia/kernel/kernel_state.cc:153-160, 489-504).
typedef struct {
	void (*notification_routine)(void);
	uint32_t priority;
	uint32_t list_entry[2];
} EX_TITLE_TERMINATE_REGISTRATION;

// xenia:src/xenia/xbox.h:39-68
enum {
	STATUS_SUCCESS = 0x00000000,
	STATUS_TIMEOUT = 0x00000102,
	STATUS_NO_MORE_FILES = (int32_t)0x80000006,
	STATUS_UNSUCCESSFUL = (int32_t)0xC0000001,
	STATUS_NOT_IMPLEMENTED = (int32_t)0xC0000002,
	STATUS_INVALID_HANDLE = (int32_t)0xC0000008,
	STATUS_INVALID_PARAMETER = (int32_t)0xC000000D,
	STATUS_NO_SUCH_FILE = (int32_t)0xC000000F,
	STATUS_END_OF_FILE = (int32_t)0xC0000011,
	STATUS_NO_MEMORY = (int32_t)0xC0000017,
	STATUS_ACCESS_DENIED = (int32_t)0xC0000022,
	STATUS_BUFFER_TOO_SMALL = (int32_t)0xC0000023,
	STATUS_OBJECT_NAME_INVALID = (int32_t)0xC0000033,
	STATUS_OBJECT_NAME_NOT_FOUND = (int32_t)0xC0000034,
	STATUS_OBJECT_NAME_COLLISION = (int32_t)0xC0000035,
	STATUS_FILE_IS_A_DIRECTORY = (int32_t)0xC00000BA,
	// NT's I/O timeout, an error: the SDK's waits return it, as STATUS_TIMEOUT would pass NT_SUCCESS.
	STATUS_IO_TIMEOUT = (int32_t)0xC00000B5,
	STATUS_NOT_SUPPORTED = (int32_t)0xC00000BB,
};

// File statuses Xenia doesn't list; the values are NT's.
enum {
	STATUS_NO_MEDIA_IN_DEVICE = (int32_t)0xC0000013,
	STATUS_DISK_FULL = (int32_t)0xC000007F,
	STATUS_OBJECT_PATH_NOT_FOUND = (int32_t)0xC000003A,
	STATUS_SHARING_VIOLATION = (int32_t)0xC0000043,
	STATUS_DIRECTORY_NOT_EMPTY = (int32_t)0xC0000101,
	STATUS_NOT_A_DIRECTORY = (int32_t)0xC0000103,
	// A drive with nothing in it: a USB port or memory unit slot (results/2026-10-20-milestone-54/probe_drives.log).
	STATUS_DEVICE_REMOVED = (int32_t)0xC00002B6,
};

// Win32 errors that XAM returns (xenia:src/xenia/xbox.h:86-104).
enum {
	ERROR_SUCCESS = 0,
	ERROR_NO_MORE_FILES = 0x12,
	ERROR_INVALID_PARAMETER = 0x57,
	// XamGetOverlappedResult's answer while a call runs (results/2026-10-19-milestone-51/probe_xam.log).
	ERROR_IO_INCOMPLETE = 0x3E4,
	ERROR_IO_PENDING = 0x3E5,
	ERROR_DEVICE_NOT_CONNECTED = 0x48F,
	ERROR_NO_SUCH_USER = 0x525,
};

// xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc:357
static const uint32_t TLS_OUT_OF_INDEXES = 0xFFFFFFFF;

// Root directory for paths like "game:\file" (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc:129).
static const HANDLE OB_DOS_DEVICES = 0xFFFFFFFD;

// xenia:src/xenia/xbox.h:126-152
enum {
	MEM_COMMIT = 0x00001000,
	MEM_RESERVE = 0x00002000,
	// In MmAllocatePhysicalMemoryEx's protection, asks for 64 KB pages (xboxkrnl/xboxkrnl_memory.cc:356-361).
	MEM_LARGE_PAGES = 0x20000000,
	PAGE_READWRITE = 0x00000004,
	PAGE_NOCACHE = 0x00000200,
	PAGE_WRITECOMBINE = 0x00000400,
};

// xenia:src/xenia/base/filesystem.h:64-73, src/xenia/vfs/entry.h:47-73, src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc:30-40
enum {
	GENERIC_READ = (int32_t)0x80000000,
	GENERIC_WRITE = 0x40000000,
	// Synchronous IO needs it (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc:152); the value is NT's.
	SYNCHRONIZE = 0x00100000,
	// Needed to mark a file for deletion; the value is NT's.
	DELETE = 0x00010000,
	FILE_ATTRIBUTE_DIRECTORY = 0x00000010,
	FILE_ATTRIBUTE_NORMAL = 0x00000080,
	// NtCreateFile's share access; the values are NT's. Xenia ignores it (xboxkrnl/xboxkrnl_io.cc:102-145), and the
	// console refuses a second open the first one doesn't share (docs/HARDWARE.md, "The log, read while it's open").
	FILE_SHARE_READ = 0x00000001,
	FILE_SHARE_WRITE = 0x00000002,
	FILE_OPEN = 1,
	FILE_CREATE = 2,
	FILE_OPEN_IF = 3,
	FILE_OVERWRITE = 4,
	FILE_OVERWRITE_IF = 5,
	FILE_DIRECTORY_FILE = 0x00000001,
	FILE_SYNCHRONOUS_IO_NONALERT = 0x00000020,
	FILE_NON_DIRECTORY_FILE = 0x00000040,
};

// File information classes (xenia:src/xenia/kernel/info/file.h:19-55).
enum {
	FILE_RENAME_INFORMATION_CLASS = 10,
	FILE_DISPOSITION_INFORMATION_CLASS = 13,
	FILE_END_OF_FILE_INFORMATION_CLASS = 20,
	FILE_NETWORK_OPEN_INFORMATION_CLASS = 34,
};

// Volume information classes (xenia:src/xenia/kernel/info/volume.h:17-22).
enum {
	FILE_FS_SIZE_INFORMATION_CLASS = 3,
};

// ExGetXConfigSetting's user category, its language and its country, one byte
// (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_xconfig.cc:66-96). xsdk reads the time zone with GetTimeZoneInformation.
enum {
	XCONFIG_USER_CATEGORY = 3,
	XCONFIG_USER_LANGUAGE = 9,
	XCONFIG_USER_COUNTRY = 14,
};

// Win32's, which XAM's GetTimeZoneInformation fills in; Xenia has no code for it, so the layout is Wine's
// (wine:include/minwinbase.h:58-67, wine:include/timezoneapi.h:39-53, at 840a939), as XAM filled its 172 bytes on the
// console. A date with year 0 recurs: day is the week in the month, 5 the last, and day_of_week 0 Sunday
// (wine:dlls/kernelbase/locale.c:4300-4328), as US Eastern's read on the console
// (results/2026-10-20-milestone-54/step3_xsdk_test_eastern.log).
typedef struct {
	uint16_t year;
	uint16_t month;
	uint16_t day_of_week;
	uint16_t day;
	uint16_t hour;
	uint16_t minute;
	uint16_t second;
	uint16_t milliseconds;
} SYSTEMTIME;

enum { TIME_ZONE_NAME_CHARS = 32 };

// Biases in minutes west of UTC, a name UTF-16; standard_date is when standard time starts, in daylight time, and
// daylight_date when daylight time starts, in standard time; month 0 for no daylight saving.
typedef struct {
	int32_t bias;
	uint16_t standard_name[TIME_ZONE_NAME_CHARS];
	SYSTEMTIME standard_date;
	int32_t standard_bias;
	uint16_t daylight_name[TIME_ZONE_NAME_CHARS];
	SYSTEMTIME daylight_date;
	int32_t daylight_bias;
} TIME_ZONE_INFORMATION;

// GetTimeZoneInformation's answers, 0 where there's no daylight saving (wine:include/timezoneapi.h:50-53).
enum {
	TIME_ZONE_ID_STANDARD = 1,
	TIME_ZONE_ID_DAYLIGHT = 2,
};

static const uint32_t TIME_ZONE_ID_INVALID = 0xFFFFFFFF;

// Event types (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc:470) and processor modes; the values are NT's.
enum {
	NOTIFICATION_EVENT = 0,
	SYNCHRONIZATION_EVENT = 1,
	KERNEL_MODE = 0,
	USER_MODE = 1,
};

static inline bool NT_SUCCESS(NTSTATUS status)
{
	return status >= 0;
}

// xboxkrnl.exe
NTSTATUS DbgPrint(const char *format, ...);
NTSTATUS ExCreateThread(HANDLE *thread, uint32_t stack_bytes, uint32_t *thread_id, void *xapi_thread_startup,
	void *start_address, void *start_context, uint32_t creation_flags);
// Copies a console setting into buffer and gives its size (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_xconfig.cc:44-139).
NTSTATUS ExGetXConfigSetting(
	uint16_t category, uint16_t setting, void *buffer, uint16_t buffer_bytes, uint16_t *required_bytes);
// Adds the registration's routine to those run as the title ends, or removes it where create is 0
// (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_modules.cc:188-198).
void ExRegisterTitleTerminateNotification(EX_TITLE_TERMINATE_REGISTRATION *registration, uint32_t create);
void ExTerminateThread(uint32_t exit_code);
// The running .xex's path, which the loader writes into the kernel's 256 bytes as it starts the title; custom
// dashboards read their own path there (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_module.cc:177-185,
// xenia:src/xenia/kernel/kernel_state.cc:351-360).
extern const char *const ExLoadedImageName;
NTSTATUS KeDelayExecutionThread(uint32_t processor_mode, uint32_t alertable, const int64_t *interval);
// Xenia returns it as 32 bits (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc:312-315).
uint64_t KeQueryPerformanceFrequency(void);
// 100 ns units since 1601, as NT counts.
void KeQuerySystemTime(uint64_t *time);
// Places a thread, given by its thread object, on the hardware threads the mask names, one bit each; the mask it had
// comes back through previous (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc:240-256).
NTSTATUS KeSetAffinityThread(void *thread, uint32_t affinity, uint32_t *previous);
// Returns TLS_OUT_OF_INDEXES when no slot is free.
uint32_t KeTlsAlloc(void);
uint32_t KeTlsFree(uint32_t slot);
void *KeTlsGetValue(uint32_t slot);
uint32_t KeTlsSetValue(uint32_t slot, void *value);
void *MmAllocatePhysicalMemoryEx(uint32_t flags, uint32_t size_bytes, uint32_t protect, uint32_t min_address,
	uint32_t max_address, uint32_t alignment);
void MmFreePhysicalMemory(uint32_t type, void *base);
// The address the GPU uses for memory from MmAllocatePhysicalMemoryEx.
uint32_t MmGetPhysicalAddress(const void *address);
// size must be the struct's; Xenia refuses any other (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc:483).
NTSTATUS MmQueryStatistics(MM_QUERY_STATISTICS_RESULT *statistics);
NTSTATUS NtAllocateVirtualMemory(
	void **base, uint32_t *size_bytes, uint32_t allocation_type, uint32_t protect, uint32_t debug_memory);
NTSTATUS NtClose(HANDLE handle);
// A real handle for the caller too (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_ob.cc:180-203); options 0 keeps source.
NTSTATUS NtDuplicateObject(HANDLE source, HANDLE *destination, uint32_t options);
NTSTATUS NtCreateEvent(HANDLE *event, const OBJECT_ATTRIBUTES *attributes, uint32_t event_type, uint32_t initial_state);
NTSTATUS NtCreateFile(HANDLE *handle, uint32_t desired_access, const OBJECT_ATTRIBUTES *attributes,
	IO_STATUS_BLOCK *io_status, const uint64_t *allocation_size, uint32_t file_attributes, uint32_t share_access,
	uint32_t creation_disposition, uint32_t create_options);
NTSTATUS NtCreateSemaphore(HANDLE *semaphore, const OBJECT_ATTRIBUTES *attributes, int32_t count, int32_t limit);
// Xenia does nothing with it (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_io.cc:591-600).
NTSTATUS NtFlushBuffersFile(HANDLE file, IO_STATUS_BLOCK *io_status);
// file_name, a pattern, is read only when a scan starts (xenia:src/xenia/kernel/xfile.cc:50-71).
NTSTATUS NtQueryDirectoryFile(HANDLE file, HANDLE event, void *apc_routine, void *apc_context,
	IO_STATUS_BLOCK *io_status, FILE_DIRECTORY_INFORMATION *information, uint32_t length_bytes,
	const ANSI_STRING *file_name, uint32_t restart_scan);
NTSTATUS NtQueryFullAttributesFile(const OBJECT_ATTRIBUTES *attributes, FILE_NETWORK_OPEN_INFORMATION *information);
NTSTATUS NtQueryInformationFile(
	HANDLE file, IO_STATUS_BLOCK *io_status, void *information, uint32_t length_bytes, uint32_t information_class);
// xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_io_info.cc:280-345
NTSTATUS NtQueryVolumeInformationFile(
	HANDLE file, IO_STATUS_BLOCK *io_status, void *information, uint32_t length_bytes, uint32_t information_class);
NTSTATUS NtReadFile(HANDLE file, HANDLE event, void *apc_routine, void *apc_context, IO_STATUS_BLOCK *io_status,
	void *buffer, uint32_t length_bytes, const uint64_t *byte_offset);
NTSTATUS NtReleaseSemaphore(HANDLE semaphore, int32_t release_count, int32_t *previous_count);
NTSTATUS NtSetEvent(HANDLE event, int32_t *previous_state);
NTSTATUS NtSetInformationFile(HANDLE file, IO_STATUS_BLOCK *io_status, const void *information, uint32_t length_bytes,
	uint32_t information_class);
NTSTATUS NtWaitForSingleObjectEx(HANDLE object, uint32_t wait_mode, uint32_t alertable, const int64_t *timeout);
NTSTATUS NtWriteFile(HANDLE file, HANDLE event, void *apc_routine, void *apc_context, IO_STATUS_BLOCK *io_status,
	const void *buffer, uint32_t length_bytes, const uint64_t *byte_offset);
NTSTATUS NtYieldExecution(void);
void RtlEnterCriticalSection(RTL_CRITICAL_SECTION *section);
void RtlInitializeCriticalSection(RTL_CRITICAL_SECTION *section);
void RtlLeaveCriticalSection(RTL_CRITICAL_SECTION *section);
// Returns nonzero when it took the section.
uint32_t RtlTryEnterCriticalSection(RTL_CRITICAL_SECTION *section);
uint32_t XAudioRegisterRenderDriverClient(const XAUDIO_RENDER_DRIVER_CLIENT *client, uint32_t *driver);
uint32_t XAudioSubmitRenderDriverFrame(uint32_t driver, const float *samples);
uint32_t XAudioUnregisterRenderDriverClient(uint32_t driver);
// Hands an event and its data to XAM's graphics notification routine, then to any registered: event 1 describes the
// title's display, 64 bytes (docs/HARDWARE.md, "The Guide shows through VdSwap").
void VdCallGraphicsNotificationRoutines(uint32_t event, const void *data);
// The GPU writes its read pointer, in dwords, to this address (xenia:src/xenia/gpu/command_processor.cc:243-246).
void VdEnableRingBufferRPtrWriteBack(uint32_t physical_address, uint32_t block_size_log2);
void VdGetCurrentDisplayInformation(DISPLAY_INFO *info);
// Copies the system command buffer XAM last set, 0x94 bytes, and gives the value VdSwap's fence is to write: a count
// up 2 a call (docs/HARDWARE.md, "The Guide shows through VdSwap").
void VdGetSystemCommandBuffer(void *buffer, uint32_t *value);
// Each points to a kernel variable holding a D3D device (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc:442-457).
extern uint32_t *const VdGlobalDevice;
extern uint32_t *const VdGlobalXamDevice;
// Runs the graphics notification routines, through which XAM's D3D device gives up the GPU (docs/HARDWARE.md).
uint32_t VdInitializeEngines(
	uint32_t flags, void *callback, void *context, const uint32_t *pfp_microcode, const uint32_t *me_microcode);
// size_log2 is log2 of the ring's size in 8-byte units (xenia:src/xenia/gpu/command_processor.cc:312).
void VdInitializeRingBuffer(uint32_t physical_address, uint32_t size_log2);
void VdQueryVideoMode(VIDEO_MODE *mode);
// Bit 30 always set, bits 28-29 the colour space (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_video.cc:166-183).
uint32_t VdSetDisplayMode(uint32_t flags);
// The kernel keeps one callback and calls it with source 0 at vblank (xenia:src/xenia/gpu/graphics_system.cc:234-272).
typedef void (*VdGraphicsInterruptCallback)(uint32_t source, void *context);
void VdSetGraphicsInterruptCallback(VdGraphicsInterruptCallback callback, void *context);
// Where VdSwap's fences land, which XAM reads to know the GPU is done with a system command buffer; NULL for none
// (docs/HARDWARE.md, "The Guide shows through VdSwap").
void VdSetSystemCommandBufferGpuIdentifierAddress(volatile uint32_t *address);
void VdShutdownEngines(void);
// Fills 64 dwords reserved in the ring: while XAM draws the Guide, the front buffer as texture fetch constant 15 and a
// call to the system command buffer, then always a fence writing system_value to system_writeback. While XAM's surface
// is to show instead of the front buffer, it replaces the front buffer's address, format, colour space and size with
// that surface's (docs/HARDWARE.md, "The Guide shows through VdSwap").
void VdSwap(volatile uint32_t *ring_space, const uint32_t *fetch_constant, volatile uint32_t *system_writeback,
	void *system_buffer, uint32_t system_value, uint32_t *front_buffer, uint32_t *texture_format, uint32_t *color_space,
	uint32_t *width, uint32_t *height);
void XeCryptRandom(void *buffer, uint32_t length_bytes);
// Returns 0 if a module like "xboxkrnl.exe" is loaded (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_modules.cc:42-62).
uint32_t XexGetModuleHandle(const char *module_name, HANDLE *module);
// A resource the .xex names, such as the title's name under its title ID in hex (xenia:src/xenia/kernel/xboxkrnl/
// xboxkrnl_modules.cc:64-84).
NTSTATUS XexGetModuleSection(HANDLE module, const char *name, const void **data, uint32_t *size);
NTSTATUS XexGetProcedureAddress(HANDLE module, uint32_t ordinal, void **address);

// Milestone 55: Windows-style threading and memory (docs/scope/55-windows-threads-and-memory.md).

// Wait types and timer types (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc:829, 698-708); the values are NT's.
enum {
	MAXIMUM_WAIT_OBJECTS = 64, // results/2026-10-20-milestone-55/probe_objects.log
	WAIT_ALL = 0,
	WAIT_ANY = 1,
	NOTIFICATION_TIMER = 0,
	SYNCHRONIZATION_TIMER = 1,
};

// More MEM_ and PAGE_ flags (xenia:src/xenia/xbox.h:126-152); the values are NT's.
enum {
	MEM_DECOMMIT = 0x00004000,
	MEM_RELEASE = 0x00008000,
	PAGE_NOACCESS = 0x00000001,
	PAGE_READONLY = 0x00000002,
};

// Wait and mutant statuses (xenia:src/xenia/xbox.h:36-63); a wait-any returns STATUS_WAIT_0 plus the index.
enum {
	STATUS_WAIT_0 = 0x00000000,
	STATUS_ABANDONED_WAIT_0 = 0x00000080,
	STATUS_USER_APC = 0x000000C0,
	STATUS_ALERTED = 0x00000101,
	STATUS_MUTANT_NOT_OWNED = (int32_t)0xC0000046,
};

// Xenia's query layout; console ABI under probe (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc:288-296).
typedef struct {
	uint32_t base_address;
	uint32_t allocation_base;
	uint32_t allocation_protect;
	uint32_t region_size;
	uint32_t state;
	uint32_t protect;
	uint32_t type;
} MEMORY_BASIC_INFORMATION;

// A singly-linked list entry and its 8-byte header, swapped whole by a double-width compare-and-swap, so the header
// must be 8-byte aligned (xenia:src/xenia/xbox.h:278-289). next holds a guest address.
typedef struct {
	uint32_t next;
} SINGLE_LIST_ENTRY;

typedef struct {
	uint32_t next;
	uint16_t depth;
	uint16_t sequence;
} __attribute__((aligned(8))) SLIST_HEADER;

// The loader puts the thread object type address in this import slot; pass its value as object_type.
extern void *const ExThreadObjectType;
// Waits on several objects at once: wait_type WAIT_ALL or WAIT_ANY, which returns STATUS_WAIT_0 plus the signalled
// object's index (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_threading.cc:878-887).
NTSTATUS NtWaitForMultipleObjectsEx(uint32_t count, const HANDLE *handles, uint32_t wait_type, uint32_t wait_mode,
	uint32_t alertable, const int64_t *timeout);
// Sets one object and waits on another atomically (xboxkrnl_threading.cc:889-912).
NTSTATUS NtSignalAndWaitForSingleObjectEx(
	HANDLE signal_object, HANDLE wait_object, uint32_t alertable, uint32_t unused, const int64_t *timeout);
NTSTATUS NtClearEvent(HANDLE event);
NTSTATUS NtPulseEvent(HANDLE event, int32_t *previous_state);
NTSTATUS NtCreateMutant(HANDLE *mutant, const OBJECT_ATTRIBUTES *attributes, uint32_t initial_owner);
// Xenia asserts the second argument 0 and ignores it (xboxkrnl_threading.cc:672-696).
NTSTATUS NtReleaseMutant(HANDLE mutant, uint32_t unused);
NTSTATUS NtCreateTimer(HANDLE *timer, const OBJECT_ATTRIBUTES *attributes, uint32_t timer_type);
// due_time negative is relative, in 100 ns units; routine NULL for no APC, then unk_one 1, resume 0, unk_zero 0
// (xboxkrnl_threading.cc:734-758).
NTSTATUS NtSetTimerEx(HANDLE timer, const int64_t *due_time, void *routine, uint32_t unk_one, void *routine_arg,
	uint32_t resume, uint32_t period_ms, uint32_t unk_zero);
NTSTATUS NtCancelTimer(HANDLE timer, uint32_t *current_state);
// Take the native thread object pointer, as ObReferenceObjectByHandle gives it, not a handle; the increment maps onto
// the kernel's levels (xboxkrnl_threading.cc:260-284). The set returns the previous priority.
int32_t KeQueryBasePriorityThread(void *thread);
int32_t KeSetBasePriorityThread(void *thread, int32_t increment);
NTSTATUS NtFreeVirtualMemory(void **base, uint32_t *size_bytes, uint32_t free_type, uint32_t debug_memory);
NTSTATUS NtProtectVirtualMemory(
	void **base, uint32_t *size_bytes, uint32_t protect, uint32_t *old_protect, uint32_t debug_memory);
// Returns the pages' protection, 0 if none (xboxkrnl_memory.cc:413-423).
uint32_t MmQueryAddressProtect(uint32_t base_address);
void MmSetAddressProtect(void *base_address, uint32_t region_size, uint32_t protect);
// The singly-linked list operations return a guest address: the old head, the popped entry, or the first flushed
// (xboxkrnl_threading.cc:1320-1392). Commonly inlined, so a title may use lwarx/stwcx. instead.
uint32_t InterlockedPushEntrySList(SLIST_HEADER *list, SINGLE_LIST_ENTRY *entry);
uint32_t InterlockedPopEntrySList(SLIST_HEADER *list);
uint32_t InterlockedFlushSList(SLIST_HEADER *list);
// The native object pointer for a handle, into out_object; object_type is ExThreadObjectType, or 0 to skip
// the check (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_ob.cc:84-116). ObDereferenceObject releases it.
NTSTATUS ObReferenceObjectByHandle(HANDLE handle, uint32_t object_type, uint32_t *out_object);
NTSTATUS ObDereferenceObject(uint32_t object);

// XAM's sockets are Winsock's calls with the caller's type first, a title's XNCALLER_TITLE
// (xenia:src/xenia/kernel/xam/xam_net.cc:42-47); their sockets are XAM's handles, and their errors Winsock's.
enum {
	XNCALLER_TITLE = 1,
	XNET_DNS_ADDRESSES = 8,
	WSA_FD_SET_SOCKETS = 64,
};

// XNDNS, which NetDll_XNetDnsLookup fills in, status 0 once found (xam_net.cc:60-64).
typedef struct {
	int32_t status;
	uint32_t count;
	uint32_t addresses[XNET_DNS_ADDRESSES];
} XNDNS;

// Winsock's fd_set and timeval, as NetDll_select reads them (xam_net.cc:818-821).
typedef struct {
	uint32_t count;
	int32_t sockets[WSA_FD_SET_SOCKETS];
} WSA_FD_SET;

typedef struct {
	int32_t seconds;
	int32_t microseconds;
} WSA_TIMEVAL;

// An asynchronous XAM call returns ERROR_IO_PENDING, then writes result, extended_error and length and sets event
// (xenia:src/xenia/xbox.h:176-184, xenia:src/xenia/kernel/kernel_state.cc:696-720).
typedef struct {
	uint32_t result;
	uint32_t length;
	uint32_t context;
	HANDLE event;
	uint32_t completion_routine;
	uint32_t completion_context;
	uint32_t extended_error;
} XAM_OVERLAPPED;

// A content package: file_name names it on its device (xenia:src/xenia/kernel/xam/content_manager.h:39-55).
typedef struct {
	uint32_t device_id;
	uint32_t content_type;
	uint16_t display_name[128];
	char file_name[42];
	uint8_t padding[2];
} XCONTENT_DATA;

// xenia:src/xenia/kernel/xam/xam_content_device.cc:101-111
typedef struct {
	uint32_t device_id;
	uint32_t device_type;
	uint64_t total_bytes;
	uint64_t free_bytes;
	uint16_t name[28];
} CONTENT_DEVICE_DATA;

// xenia:src/xenia/kernel/xam/xam_user.cc:75-83
typedef struct {
	uint64_t xuid;
	uint32_t unknown_08;
	uint32_t signin_state;
	uint32_t unknown_10;
	uint32_t unknown_14;
	char name[16];
} USER_SIGNIN_INFO;

// xam.xex
int32_t NetDll_accept(uint32_t caller, int32_t socket, void *address, int32_t *length);
int32_t NetDll_bind(uint32_t caller, int32_t socket, const void *address, int32_t length);
int32_t NetDll_closesocket(uint32_t caller, int32_t socket);
int32_t NetDll_connect(uint32_t caller, int32_t socket, const void *address, int32_t length);
int32_t NetDll_getpeername(uint32_t caller, int32_t socket, void *address, int32_t *length);
int32_t NetDll_getsockname(uint32_t caller, int32_t socket, void *address, int32_t *length);
int32_t NetDll_getsockopt(uint32_t caller, int32_t socket, int32_t level, int32_t name, void *value, int32_t *length);
int32_t NetDll_ioctlsocket(uint32_t caller, int32_t socket, uint32_t command, uint32_t *argument);
int32_t NetDll_listen(uint32_t caller, int32_t socket, int32_t backlog);
int32_t NetDll_recv(uint32_t caller, int32_t socket, void *buffer, int32_t length, int32_t flags);
int32_t NetDll_recvfrom(
	uint32_t caller, int32_t socket, void *buffer, int32_t length, int32_t flags, void *from, int32_t *from_length);
int32_t NetDll_select(
	uint32_t caller, int32_t count, WSA_FD_SET *read, WSA_FD_SET *write, WSA_FD_SET *error, const WSA_TIMEVAL *timeout);
int32_t NetDll_send(uint32_t caller, int32_t socket, const void *buffer, int32_t length, int32_t flags);
int32_t NetDll_sendto(uint32_t caller, int32_t socket, const void *buffer, int32_t length, int32_t flags,
	const void *to, int32_t to_length);
int32_t NetDll_setsockopt(
	uint32_t caller, int32_t socket, int32_t level, int32_t name, const void *value, int32_t length);
int32_t NetDll_shutdown(uint32_t caller, int32_t socket, int32_t how);
int32_t NetDll_socket(uint32_t caller, int32_t family, int32_t type, int32_t protocol);
int32_t NetDll_WSAGetLastError(void);
int32_t NetDll_WSAStartup(uint32_t caller, uint16_t version, void *data);
// Starts the lookup and gives its XNDNS, which XAM fills in and sets event, if one is given, when done
// (xam_net.cc:542-560).
int32_t NetDll_XNetDnsLookup(uint32_t caller, const char *name, HANDLE event, XNDNS **result);
int32_t NetDll_XNetDnsRelease(uint32_t caller, XNDNS *result);
// params is XNetStartupParams, 13 bytes, its first its size (xam_net.cc:154-168).
int32_t NetDll_XNetStartup(uint32_t caller, const uint8_t *params);
// The console's time zone into info, and TIME_ZONE_ID_DAYLIGHT, _STANDARD or _UNKNOWN for which time it is now
// (wine:include/timezoneapi.h:39-58); XAM exports it (xenia:src/xenia/kernel/xam/xam_table.inc:883).
uint32_t GetTimeZoneInformation(TIME_ZONE_INFORMATION *info);
// The content calls (xenia:src/xenia/kernel/xam/xam_content.cc, xam_content_device.cc). A package mounts under root,
// "save" as save:, until closed.
uint32_t XamContentClose(const char *root, XAM_OVERLAPPED *overlapped);
uint32_t XamContentCreateDeviceEnumerator(
	uint32_t content_type, uint32_t flags, uint32_t items, uint32_t *buffer_bytes, HANDLE *enumerator);
uint32_t XamContentCreateEnumerator(uint32_t user_index, uint32_t device_id, uint32_t content_type, uint32_t flags,
	uint32_t items, uint32_t *buffer_bytes, HANDLE *enumerator);
uint32_t XamContentCreateEx(uint32_t user_index, const char *root, const XCONTENT_DATA *data, uint32_t flags,
	uint32_t *disposition, uint32_t *license_mask, uint32_t cache_bytes, uint64_t content_bytes,
	XAM_OVERLAPPED *overlapped);
uint32_t XamContentDelete(uint32_t user_index, const XCONTENT_DATA *data, XAM_OVERLAPPED *overlapped);
uint32_t XamContentFlush(const char *root, XAM_OVERLAPPED *overlapped);
uint32_t XamContentGetDeviceData(uint32_t device_id, CONTENT_DEVICE_DATA *data);
uint32_t XamContentGetDeviceName(uint32_t device_id, uint16_t *name, uint32_t name_chars);
uint32_t XamContentGetDeviceState(uint32_t device_id, XAM_OVERLAPPED *overlapped);
// The next items into buffer; ERROR_NO_MORE_FILES past the last. NtClose ends it (xenia:src/xenia/kernel/xam/
// xam_enum.cc:30-84).
uint32_t XamEnumerate(HANDLE enumerator, uint32_t flags, void *buffer, uint32_t buffer_bytes, uint32_t *items,
	XAM_OVERLAPPED *overlapped);
// A number in XAM's list of locales, a country each: 35 Britain, 36 the US
// (xenia:src/xenia/kernel/xam/xam_locale.cc:110-119). XamGetLocale asks for countries up to 111 and locales up to 43
// (xam_locale.cc:203-225).
uint32_t XamGetLocaleEx(uint32_t max_country, uint32_t max_locale);
uint32_t XamGetOverlappedResult(XAM_OVERLAPPED *overlapped, uint32_t *length, uint32_t wait);
uint32_t XamInputGetState(uint32_t user_index, uint32_t flags, INPUT_STATE *state);
uint32_t XamInputSetState(uint32_t user_index, uint32_t flags, const INPUT_VIBRATION *vibration);
uint32_t XamIsUIActive(void);
void XamLoaderTerminateTitle(void);
// A listener for the notification areas whose bits mask sets, an area being bits 25-30 of a notification's id; its
// handle, or 0 (xenia:src/xenia/kernel/xam/xam_notify.cc:21-41, xnotifylistener.h:25-32).
HANDLE XamNotifyCreateListener(uint64_t mask, uint32_t max_version);
// The dialogs, their text UTF-16 (xenia:src/xenia/kernel/xam/xam_ui.cc:268-463, xam_user.cc:518-530).
uint32_t XamShowKeyboardUI(uint32_t user_index, uint32_t flags, const uint16_t *default_text, const uint16_t *title,
	const uint16_t *description, uint16_t *text, uint32_t text_chars, XAM_OVERLAPPED *overlapped);
uint32_t XamShowMessageBoxUI(uint32_t user_index, const uint16_t *title, const uint16_t *text, uint32_t button_count,
	const uint16_t *const *buttons, uint32_t focus_button, uint32_t flags, uint32_t *pressed,
	XAM_OVERLAPPED *overlapped);
uint32_t XamShowSigninUI(uint32_t panes, uint32_t flags);
// The profile calls (xenia:src/xenia/kernel/xam/xam_user.cc:29-310).
uint32_t XamUserGetName(uint32_t user_index, char *name, uint32_t name_bytes);
uint32_t XamUserGetSigninInfo(uint32_t user_index, uint32_t flags, USER_SIGNIN_INFO *info);
uint32_t XamUserGetSigninState(uint32_t user_index);
uint32_t XamUserGetXUID(uint32_t user_index, uint32_t type_mask, uint64_t *xuid);
uint32_t XamUserReadProfileSettings(uint32_t title_id, uint32_t user_index, uint32_t xuid_count, const uint64_t *xuids,
	uint32_t setting_count, const uint32_t *setting_ids, uint32_t *buffer_bytes, void *buffer,
	XAM_OVERLAPPED *overlapped);
// Xenia gives English (xenia:src/xenia/kernel/xam/xam_info.cc:208-223).
uint32_t XGetLanguage(void);
// Takes the listener's next notification, or its next match_id one if that's nonzero; nonzero if there was one
// (xenia:src/xenia/kernel/xam/xam_notify.cc:53-91).
uint32_t XNotifyGetNext(HANDLE listener, uint32_t match_id, uint32_t *id, uint32_t *param);

#ifdef __cplusplus
}
#endif

#endif
