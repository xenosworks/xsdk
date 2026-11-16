#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kernel_io.h"
#include "xsdk/exfat.h"
#include "xsdk/kernel.h"
#include "xsdk/plugin.h"

// Milestone 77, step 3 (docs/scope/77-exfat.md): exFAT on the USB mass-storage devices as a kernel file system. The
// module is a plugin (docs/GUIDE.md, "Plugins") whose thread looks at \Device\Mass0 to \Device\Mass2 every 500 ms and
// mounts a volume device of its own, driven by this file's DRIVER_OBJECT, on each one holding an exFAT volume
// (kernel_seam.md, section 6); the file system is src/exfat.c, over read and write IRPs to the device. The kernel
// routes every open under the device to IRP_MJ_CREATE here and the calls on the handles it hands out to the other
// major functions (section 4). The answers follow the kernel's own FAT driver's, read from its disassembly: the
// addresses in the comments are its handlers in kernel 17559.
// Milestone 78, step 1 (docs/scope/78-exfat-hdd.md): the drive's content partition, \Device\Harddisk0\Partition1, is
// a fourth device, mounted the same way when exfat_format.xex has formatted it and left alone while it is FATX. What
// its device holds from the gate on is sampled at every poll and logged once the log can be written, which on an
// exFAT partition is after the mount: that timeline is the record the partition's mount point is chosen from.

#define PARTITION_DEVICE "\\Device\\Harddisk0\\Partition1"
#define XSDK_FOLDER      PARTITION_DEVICE "\\XSDK"
#define LOG_FOLDER       XSDK_FOLDER "\\exfat"
static const char LOG_PATH[] = LOG_FOLDER "\\exfat.log";
// Their presence beside the log unloads the module and logs every request.
static const char UNLOAD_PATH[] = LOG_FOLDER "\\exfat_unload.txt";
static const char TRACE_PATH[] = LOG_FOLDER "\\exfat_trace.txt";
// The same two under the volume, as the module looks them up itself (flag_present).
static const char UNLOAD_NAME[] = "XSDK\\exfat\\exfat_unload.txt";
static const char TRACE_NAME[] = "XSDK\\exfat\\exfat_trace.txt";

enum {
	VOLUMES_MAX = 4,
	PARTITION = 3,
	// The boot timeline keeps this many changes of what the partition's device holds.
	TIMELINE_MAX = 32,
	FILES_MAX = 64,
	SEARCHES_MAX = 32,
	// A volume-relative path, components separated by '\', with its terminator.
	PATH_BYTES = 520,
	POLL_MS = 500,
	// Dirty metadata goes to the stick at a poll with no request since the last, or after this many busy polls.
	SYNC_POLLS_MAX = 10,
	// One read or write IRP to the device moves this much at most: raw 1 MiB IRPs ran as fast as 64 KB ones, and
	// each IRP from here costs more than one from a title (results/2026-11-14-milestone-77-step-3/speed9).
	TRANSFER_BYTES_MAX = 1 << 20,
	// An unaligned buffer goes through the bounce buffer, this much an IRP.
	BOUNCE_BYTES = 65536,
	// The time base runs at 49.875 MHz (docs/HARDWARE.md).
	TIME_BASE_TICKS_PER_MS = 49875,
	SUMMARY_BYTES_MIB = 64,
	// The kernel's raw file system, which a device open attaches when no file system takes the device
	// (kernel_seam.md, section 3).
	KERNEL_RAW_DRIVER = 0x80043224,
	// The kernel's storage announcement to xam (0x8012bca8: links a StorageRecord into its list, calls xam's callback;
	// xam's DrvGetContentStorageNotification copies the record's six words out; the BuiltInMuMmc driver announces its
	// storage so, sub_8008FF90), the arrival action, and xam's type for the hard drive (its own entry at 0x8167ed90; a
	// stick is 16, a memory unit 2: results/2026-11-15-milestone-78-step-1/probe_storage_state_run11).
	KERNEL_ANNOUNCE_STORAGE = 0x8012bca8,
	STORAGE_ARRIVAL = 1,
	STORAGE_TYPE_HDD = 1,
	// xam dismounts a drive it listed as formatted before it looks at it again (announce_partition): declined for this
	// long.
	XAM_SHIELD_100NS = 2 * 10000000,
	// xam numbers content devices from this counter, handing its value out and advancing it to the next free number
	// (xam 0x8167b3c8); the drive, registered first at xam's load, is 1 on a stock console and titles take it so (Xenia
	// gives its drive 1, xenia:src/xenia/kernel/xam/xam_content_device.h:19-27). Registered again it would get the next
	// free number (results/2026-11-15-milestone-78-step-1, probe_storage_state_after_signin: 3), so announce_partition
	// sets the counter back first.
	XAM_DEVICE_ID_COUNTER = 0x81a81328,
	STORAGE_ID_HDD = 1,
	// FILE_FS_VOLUME_INFORMATION's fixed part: creation time, serial, label length, a BOOLEAN; FAT answers 17 bytes
	// with no label (0x800a2d18), so the label follows the BOOLEAN unpadded.
	FS_VOLUME_INFORMATION_BYTES = 17,
	FS_SIZE_INFORMATION_BYTES = 24,
	FS_DEVICE_INFORMATION_BYTES = 8,
	FS_ATTRIBUTE_INFORMATION_BYTES = 12,
	FILE_INTERNAL_INFORMATION_BYTES = 8,
	FILE_POSITION_INFORMATION_BYTES = 8,
};

static const char FILE_SYSTEM_NAME[] = "exFAT";

typedef struct Volume Volume;

// An open file or directory: one for every open of the same entry, as FAT keeps one FCB.
typedef struct {
	Volume *volume;
	// Its volume was dismounted under it; only CLEANUP and CLOSE still come.
	bool orphan;
	bool delete_on_close;
	bool deleted;
	ExfatNode node;
	uint32_t path_length;
	char path[PATH_BYTES];
	// Handles not yet cleaned up, and file objects not yet closed.
	uint32_t opens;
	uint32_t references;
	SHARE_ACCESS share;
} File;

// A directory listing in progress, a file object's FsContext2.
typedef struct {
	bool used;
	bool first_query;
	bool opened;
	uint32_t index;
	ExfatDirectory listing;
	uint32_t pattern_length;
	char pattern[EXFAT_NAME_MAX + 1];
} Search;

// The volume device's extension: the device first, as every kernel file system has it (kernel_seam.md, section 3).
typedef struct {
	DEVICE_OBJECT *device;
	Volume *volume;
} Extension;

struct Volume {
	const char *device_name;
	// The device object's type, mass storage for a stick and disk for the partition; another is left alone.
	uint8_t device_type;
	// Referenced by name while media is looked at or mounted, released when it goes: the kernel may create the device
	// anew at a replug.
	DEVICE_OBJECT *device;
	DEVICE_OBJECT *volume_device;
	bool mounted;
	// Sector 0 of the media now in was looked at and is not ours; looked at again once the media changes.
	bool refused;
	// Volume devices deleted but not yet gone: the kernel keeps one while file objects reference it, and calls
	// delete_device, in this image, when the last closes. The module stays loaded until then.
	uint32_t pending_deletes;
	// Requests dispatched, what the poll last saw of them, and polls since the last sync that saw new ones.
	uint32_t requests;
	uint32_t requests_seen;
	uint32_t busy_polls;
	uint32_t sector_bytes;
	uint64_t sectors;
	// The open file objects, through their fs_link.
	KERNEL_LIST_ENTRY files;
	// Read and write traffic since the last summary: requests and bytes, time base ticks in the dispatch and in the
	// device's IRPs, and those IRPs' count. Logged at a cleanup once SUMMARY_BYTES have gone by.
	uint32_t rw_requests;
	uint64_t rw_bytes;
	uint64_t dispatch_ticks;
	uint64_t io_ticks;
	uint32_t irps;
	// Requests whose buffer wasn't 4 KB aligned: whether the kernel hands the caller's buffer on or a copy.
	uint32_t unaligned_requests;
	ExfatVolume fs;
};

static const char *const device_names[VOLUMES_MAX] = { "\\Device\\Mass0", "\\Device\\Mass1", "\\Device\\Mass2",
	PARTITION_DEVICE };
static const uint8_t device_types[VOLUMES_MAX] = { DEVICE_TYPE_MASS_STORAGE, DEVICE_TYPE_MASS_STORAGE,
	DEVICE_TYPE_MASS_STORAGE, DEVICE_TYPE_DISK };
// Allocated once the thread starts: in the image, the three 269 KB structs made it 1 MB, and a module that size
// loaded at boot hung the dash where a 40 KB one booted (results/2026-11-14-milestone-77-step-4/README.md, run 22).
static Volume *volumes;
static File files[FILES_MAX];
static Search searches[SEARCHES_MAX];
// One lock over every volume: the core is not reentrant, and the poll thread mounts under it too.
static RTL_CRITICAL_SECTION lock;
static bool trace;
// Whether the log's folder exists: on an exFAT partition only once the partition is mounted.
static bool log_ready;
// A storage announcement's record, as the BuiltInMuMmc driver keeps its own at 0x801a608c: the kernel's list links,
// the device's name, the action, the type. It stays in the kernel's list until xam takes it, so it is static.
typedef struct {
	uint32_t flink;
	uint32_t blink;
	ANSI_STRING name;
	uint32_t action;
	uint32_t type;
} StorageRecord;
static StorageRecord partition_record;
// Set by the partition's mount: the thread announces the drive to xam again (announce_partition).
static bool announce_pending;
// Until when a dismount of the partition is xam's look after that announcement, and declined.
static uint64_t xam_shield_until;
// For a caller's buffer the device's alignment requirement refuses (Mass0's is 1: odd addresses); every transfer runs
// under the module's lock.
static _Alignas(4096) uint8_t bounce[BOUNCE_BYTES];

#define TRACE(...)                 \
	do {                           \
		if (trace)                 \
			log_line(__VA_ARGS__); \
	} while (0)
// A request on the module's own folder is left out: its log's writes and its polls of the flag files.
#define TRACE_FILE(path, ...)         \
	do {                              \
		if (trace && !own_file(path)) \
			log_line(__VA_ARGS__);    \
	} while (0)

static bool own_file(const char *path)
{
	return strncmp(path, LOG_FOLDER + sizeof PARTITION_DEVICE, sizeof LOG_FOLDER - sizeof PARTITION_DEVICE - 1) == 0;
}

// Lines wait here for the thread, which writes them outside the module's lock: xsdk_plugin_log writes through the
// kernel, and on the partition that is this module's volume a line logged under its lock would re-enter the module
// while the thread, writing a line of its own, waits for that lock with the logger's held (the trace froze the console
// at every boot it was on, results/2026-11-15-milestone-78-step-1, the sign-in checks).
enum { QUEUED_LINES = 128, QUEUED_LINE_BYTES = 192, MODULE_PRIORITY = 16 };
static char queued[QUEUED_LINES][QUEUED_LINE_BYTES];
static uint32_t queued_first, queued_count, queued_dropped;
static RTL_CRITICAL_SECTION queue_lock;

static void log_line(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void log_line(const char *format, ...)
{
	char line[QUEUED_LINE_BYTES];
	va_list args;
	va_start(args, format);
	(void)_vsnprintf(line, sizeof line - 1, format, args);
	va_end(args);
	line[sizeof line - 1] = 0;
	KeEnterCriticalRegion();
	RtlEnterCriticalSection(&queue_lock);
	if (queued_count < QUEUED_LINES)
		memcpy(queued[(queued_first + queued_count++) % QUEUED_LINES], line, sizeof line);
	else
		queued_dropped++;
	RtlLeaveCriticalSection(&queue_lock);
	KeLeaveCriticalRegion();
}

// The PCR (r13) keeps the running thread's KTHREAD at 0x100.
enum { PCR_THREAD = 0x100 };

static const uint8_t *pcr(void)
{
	const uint8_t *p;
	__asm__ volatile("mr %0, 13" : "=r"(p));
	return p;
}

// The thread writes the queued lines, holding no lock while it writes.
static void write_queued(void)
{
	for (;;) {
		char line[QUEUED_LINE_BYTES];
		KeEnterCriticalRegion();
		RtlEnterCriticalSection(&queue_lock);
		const bool any = queued_count != 0;
		if (any) {
			memcpy(line, queued[queued_first], sizeof line);
			queued_first = (queued_first + 1) % QUEUED_LINES;
			queued_count--;
		}
		const uint32_t dropped = any ? 0 : queued_dropped;
		if (!any)
			queued_dropped = 0;
		RtlLeaveCriticalSection(&queue_lock);
		KeLeaveCriticalRegion();
		if (!any) {
			if (dropped)
				xsdk_plugin_log("exfat: %u lines dropped, the queue was full", (unsigned)dropped);
			return;
		}
		xsdk_plugin_log("%s", line);
	}
}

static void lock_module(void)
{
	KeEnterCriticalRegion();
	RtlEnterCriticalSection(&lock);
}

static void unlock_module(void)
{
	RtlLeaveCriticalSection(&lock);
	KeLeaveCriticalRegion();
}

// Lists through the kernel's LIST_ENTRY, addresses as 32-bit words.

static uint32_t address_of(const void *pointer)
{
	return (uint32_t)(uintptr_t)pointer;
}

static void list_init(KERNEL_LIST_ENTRY *head)
{
	head->flink = address_of(head);
	head->blink = address_of(head);
}

static void list_append(KERNEL_LIST_ENTRY *head, KERNEL_LIST_ENTRY *entry)
{
	KERNEL_LIST_ENTRY *const last = (KERNEL_LIST_ENTRY *)(uintptr_t)head->blink;
	entry->flink = address_of(head);
	entry->blink = address_of(last);
	last->flink = address_of(entry);
	head->blink = address_of(entry);
}

// Takes the entry out and leaves it pointing at itself, so a second removal is harmless.
static void list_remove(KERNEL_LIST_ENTRY *entry)
{
	KERNEL_LIST_ENTRY *const next = (KERNEL_LIST_ENTRY *)(uintptr_t)entry->flink;
	KERNEL_LIST_ENTRY *const previous = (KERNEL_LIST_ENTRY *)(uintptr_t)entry->blink;
	next->blink = address_of(previous);
	previous->flink = address_of(next);
	entry->flink = address_of(entry);
	entry->blink = address_of(entry);
}

static FILE_OBJECT *file_object_of(KERNEL_LIST_ENTRY *link)
{
	return (FILE_OBJECT *)((uint8_t *)link - offsetof(FILE_OBJECT, fs_link));
}

// The device.

static uint32_t time_base(void)
{
	uint32_t low;
	__asm__ volatile("mftb %0" : "=r"(low));
	return low;
}

static NTSTATUS transfer(Volume *v, uint32_t major, uint64_t sector, uint32_t count, void *buffer)
{
	uint8_t *bytes = buffer;
	uint64_t offset = sector * v->sector_bytes;
	uint32_t left = count * v->sector_bytes;
	const bool bounced = (address_of(buffer) & v->device->alignment_requirement) != 0;
	NTSTATUS status = STATUS_SUCCESS;
	while (left && NT_SUCCESS(status)) {
		const uint32_t most = bounced ? BOUNCE_BYTES : TRANSFER_BYTES_MAX;
		const uint32_t chunk = left < most ? left : most;
		void *const data = bounced ? bounce : bytes;
		if (bounced && major == IRP_MJ_WRITE)
			memcpy(bounce, bytes, chunk);
		const uint32_t started = time_base();
		status = IoSynchronousFsdRequest(major, address_of(v->device), data, chunk, &offset);
		v->io_ticks += time_base() - started;
		v->irps++;
		if (bounced && major == IRP_MJ_READ && NT_SUCCESS(status))
			memcpy(bytes, bounce, chunk);
		bytes += chunk;
		offset += chunk;
		left -= chunk;
	}
	if (!NT_SUCCESS(status))
		log_line("%s: %s of %u sectors at %u failed: 0x%08X", v->device_name, major == IRP_MJ_READ ? "read" : "write",
			(unsigned)count, (unsigned)sector, (unsigned)status);
	return status;
}

static int device_read(void *context, uint64_t sector, uint32_t count, void *buffer)
{
	return NT_SUCCESS(transfer(context, IRP_MJ_READ, sector, count, buffer)) ? 0 : -1;
}

static int device_write(void *context, uint64_t sector, uint32_t count, const void *buffer)
{
	return NT_SUCCESS(transfer(context, IRP_MJ_WRITE, sector, count, (void *)buffer)) ? 0 : -1;
}

static uint64_t device_now(void *context)
{
	(void)context;
	uint64_t time;
	KeQuerySystemTime(&time);
	return time;
}

// The geometry IOCTL answers only with media in (kernel_seam.md, section 2); the partition IOCTL gives the whole
// stick's length in 64 bits (step 1's run).
static bool media_present(Volume *v)
{
	uint32_t geometry[2] = { 0, 0 };
	uint32_t returned = 0;
	NTSTATUS status = IoSynchronousDeviceIoControlRequest(
		IOCTL_DISK_GET_DRIVE_GEOMETRY, address_of(v->device), NULL, 0, geometry, sizeof geometry, &returned);
	const uint32_t sector_bytes = geometry[1];
	if (!NT_SUCCESS(status) || sector_bytes < 512 || sector_bytes > EXFAT_SECTOR_BYTES_MAX ||
		(sector_bytes & (sector_bytes - 1)))
		return false;
	v->sector_bytes = sector_bytes;
	v->sectors = geometry[0];
	uint32_t partition[4] = { 0, 0, 0, 0 };
	status = IoSynchronousDeviceIoControlRequest(
		IOCTL_DISK_GET_PARTITION_INFO, address_of(v->device), NULL, 0, partition, sizeof partition, &returned);
	const uint64_t length = (uint64_t)partition[2] << 32 | partition[3];
	if (NT_SUCCESS(status) && length)
		v->sectors = length / sector_bytes;
	return true;
}

// Statuses.

static NTSTATUS status_of(ExfatStatus status)
{
	switch (status) {
	case EXFAT_OK:
		return STATUS_SUCCESS;
	case EXFAT_IO:
		return STATUS_IO_DEVICE_ERROR;
	case EXFAT_NOT_EXFAT:
	case EXFAT_UNSUPPORTED:
		return STATUS_UNRECOGNIZED_VOLUME;
	case EXFAT_CORRUPT:
		return STATUS_DISK_CORRUPT_ERROR;
	case EXFAT_NOT_FOUND:
		return STATUS_OBJECT_NAME_NOT_FOUND;
	case EXFAT_EXISTS:
		return STATUS_OBJECT_NAME_COLLISION;
	case EXFAT_NOT_DIRECTORY:
		return STATUS_NOT_A_DIRECTORY;
	case EXFAT_IS_DIRECTORY:
		return STATUS_FILE_IS_A_DIRECTORY;
	case EXFAT_NOT_EMPTY:
		return STATUS_DIRECTORY_NOT_EMPTY;
	case EXFAT_FULL:
		return STATUS_DISK_FULL;
	case EXFAT_NAME:
		return STATUS_OBJECT_NAME_INVALID;
	}
	return STATUS_UNSUCCESSFUL;
}

static NTSTATUS complete(IRP *irp, NTSTATUS status, uint32_t information)
{
	irp->io_status.status = status;
	irp->io_status.information = information;
	IoCompleteRequest(irp, 0);
	return status;
}

static Volume *volume_of(DEVICE_OBJECT *volume_device)
{
	return ((Extension *)volume_device->device_extension)->volume;
}

// Under the lock: the poll holds its sync back while requests keep coming.
static void touch(DEVICE_OBJECT *volume_device)
{
	volume_of(volume_device)->requests++;
}

// Files.

static bool same_entry(const ExfatNode *a, const ExfatNode *b)
{
	return a->parent_cluster == b->parent_cluster && a->set_offset == b->set_offset;
}

static File *file_find(Volume *v, const ExfatNode *node)
{
	for (unsigned i = 0; i < FILES_MAX; i++) {
		if (files[i].volume == v && !files[i].orphan && same_entry(&files[i].node, node))
			return &files[i];
	}
	return NULL;
}

static File *file_new(Volume *v, const ExfatNode *node, const char *path, uint32_t path_length)
{
	for (unsigned i = 0; i < FILES_MAX; i++) {
		File *const f = &files[i];
		if (f->volume)
			continue;
		memset(f, 0, sizeof *f);
		f->volume = v;
		f->node = *node;
		memcpy(f->path, path, path_length);
		f->path[path_length] = 0;
		f->path_length = path_length;
		return f;
	}
	return NULL;
}

// A failed open leaves the file it found or made for nothing.
static void file_release_unused(File *f)
{
	if (f && f->references == 0)
		f->volume = NULL;
}

static bool is_directory(const File *f)
{
	return (f->node.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool is_root(const File *f)
{
	return f->node.parent_cluster == 0;
}

static bool is_separator(char c)
{
	return c == '\\' || c == '/';
}

// Where the last component of a path starts, and how long the parent before its separator is.
static void split_leaf(const char *path, uint32_t length, uint32_t *parent_length, uint32_t *leaf_start)
{
	uint32_t start = length;
	while (start && !is_separator(path[start - 1]))
		start--;
	*leaf_start = start;
	*parent_length = start ? start - 1 : 0;
}

// Whether another open file lives under this directory, as FAT refuses renaming a directory with open children.
static bool has_open_children(const File *directory)
{
	for (unsigned i = 0; i < FILES_MAX; i++) {
		const File *const f = &files[i];
		if (f->volume != directory->volume || f->orphan || f == directory || f->path_length <= directory->path_length)
			continue;
		if (directory->path_length == 0 ||
			(memcmp(f->path, directory->path, directory->path_length) == 0 &&
				is_separator(f->path[directory->path_length])))
			return true;
	}
	return false;
}

static Search *search_new(void)
{
	for (unsigned i = 0; i < SEARCHES_MAX; i++) {
		if (!searches[i].used) {
			memset(&searches[i], 0, sizeof searches[i]);
			searches[i].used = true;
			return &searches[i];
		}
	}
	return NULL;
}

static void search_free(FILE_OBJECT *fo)
{
	Search *const s = fo->fs_context2;
	if (s)
		s->used = false;
	fo->fs_context2 = NULL;
}

// What every request on a file object meets first: FAT's order (0x800a4890 and the others).
static NTSTATUS file_object_usable(const Volume *v, const FILE_OBJECT *fo)
{
	if ((fo->flags & FILE_OBJECT_DISMOUNTED) || !v->mounted)
		return STATUS_VOLUME_DISMOUNTED;
	if (fo->flags & FILE_OBJECT_CLOSED)
		return STATUS_FILE_CLOSED;
	const File *const f = fo->fs_context;
	if (f && f->deleted)
		return STATUS_FILE_DELETED;
	return STATUS_SUCCESS;
}

static uint32_t attributes_of(const ExfatNode *node)
{
	const uint32_t attributes = node->attributes & FILE_ATTRIBUTE_VALID;
	return attributes ? attributes : FILE_ATTRIBUTE_NORMAL;
}

static uint64_t allocation_of(const Volume *v, const ExfatNode *node)
{
	if (node->attributes & FILE_ATTRIBUTE_DIRECTORY)
		return 0;
	const uint64_t cluster_bytes = (uint64_t)1 << v->fs.cluster_shift;
	return (node->size_bytes + cluster_bytes - 1) & ~(cluster_bytes - 1);
}

// IRP_MJ_CREATE: FAT 0x800a51c8.

typedef struct {
	Volume *volume;
	IRP *irp;
	FILE_OBJECT *fo;
	uint32_t access;
	uint32_t disposition;
	uint32_t options;
	uint32_t share;
	uint16_t attributes;
	bool target_directory;
	bool trailing_separator;
	char path[PATH_BYTES];
	uint32_t path_length;
} Open;

// The file object takes the file: share counts, contexts, the volume's list.
static NTSTATUS attach(Open *o, File *f, uint32_t information)
{
	if (f->share.counts[0]) {
		const NTSTATUS status = IoCheckShareAccess(o->access, o->share, o->fo, &f->share, 1);
		if (!NT_SUCCESS(status))
			return status;
	} else {
		IoSetShareAccess(o->access, o->share, o->fo, &f->share);
	}
	f->opens++;
	f->references++;
	o->fo->fs_context = f;
	o->fo->fs_context2 = NULL;
	list_append(&o->volume->files, &o->fo->fs_link);
	return complete(o->irp, STATUS_SUCCESS, information);
}

static NTSTATUS open_existing_file(Open *o, File *f)
{
	const bool directory = is_directory(f);
	if (directory) {
		if (o->disposition != FILE_OPEN && o->disposition != FILE_OPEN_IF)
			return STATUS_OBJECT_NAME_COLLISION;
		if (o->options & FILE_NON_DIRECTORY_FILE)
			return STATUS_FILE_IS_A_DIRECTORY;
	} else {
		if (o->disposition == FILE_CREATE)
			return STATUS_OBJECT_NAME_COLLISION;
		if (o->trailing_separator || (o->options & FILE_DIRECTORY_FILE))
			return STATUS_NOT_A_DIRECTORY;
		if ((f->node.attributes & FILE_ATTRIBUTE_READONLY) && (o->access & (FILE_WRITE_DATA | FILE_APPEND_DATA)))
			return STATUS_ACCESS_DENIED;
		if ((f->node.attributes & FILE_ATTRIBUTE_READONLY) && (o->options & FILE_DELETE_ON_CLOSE))
			return STATUS_CANNOT_DELETE;
	}
	uint32_t information = FILE_OPENED;
	if (!directory &&
		(o->disposition == FILE_SUPERSEDE || o->disposition == FILE_OVERWRITE || o->disposition == FILE_OVERWRITE_IF)) {
		ExfatStatus status = xsdk_exfat_resize(&o->volume->fs, &f->node, 0);
		if (status == EXFAT_OK) {
			f->node.attributes = FILE_ATTRIBUTE_ARCHIVE |
				(o->attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM));
			status = xsdk_exfat_update(&o->volume->fs, &f->node);
		}
		if (status != EXFAT_OK)
			return status_of(status);
		information = FILE_OVERWRITTEN;
	}
	if (!directory && (o->options & FILE_DELETE_ON_CLOSE))
		f->delete_on_close = true;
	return attach(o, f, information);
}

static NTSTATUS open_existing(Open *o, const ExfatNode *node, const char *path, uint32_t path_length)
{
	File *f = file_find(o->volume, node);
	if (!f) {
		f = file_new(o->volume, node, path, path_length);
		if (!f)
			return STATUS_INSUFFICIENT_RESOURCES;
	}
	const NTSTATUS status = open_existing_file(o, f);
	if (!NT_SUCCESS(status))
		file_release_unused(f);
	return status;
}

// A rename's target: the last component's directory, and whether the component exists (SL_OPEN_TARGET_DIRECTORY).
static NTSTATUS open_target_directory(Open *o, bool exists)
{
	if (o->path_length == 0)
		return STATUS_INVALID_PARAMETER;
	uint32_t parent_length;
	uint32_t leaf;
	split_leaf(o->path, o->path_length, &parent_length, &leaf);
	o->path[parent_length] = 0;
	ExfatNode parent;
	const ExfatStatus status = xsdk_exfat_lookup(&o->volume->fs, o->path, &parent);
	if (status != EXFAT_OK)
		return status == EXFAT_NOT_FOUND || status == EXFAT_NOT_DIRECTORY ? STATUS_OBJECT_PATH_NOT_FOUND
																		  : status_of(status);
	if (!(parent.attributes & FILE_ATTRIBUTE_DIRECTORY))
		return STATUS_OBJECT_PATH_NOT_FOUND;
	File *f = file_find(o->volume, &parent);
	if (!f) {
		f = file_new(o->volume, &parent, o->path, parent_length);
		if (!f)
			return STATUS_INSUFFICIENT_RESOURCES;
	}
	const NTSTATUS attached = attach(o, f, exists ? FILE_EXISTS : FILE_DOES_NOT_EXIST);
	if (!NT_SUCCESS(attached))
		file_release_unused(f);
	return attached;
}

static NTSTATUS create_new(Open *o)
{
	if (o->disposition == FILE_OPEN || o->disposition == FILE_OVERWRITE)
		return STATUS_OBJECT_NAME_NOT_FOUND;
	const bool directory = (o->options & FILE_DIRECTORY_FILE) != 0;
	if (o->trailing_separator && (o->options & FILE_NON_DIRECTORY_FILE))
		return STATUS_OBJECT_NAME_INVALID;
	ExfatNode node;
	ExfatStatus status = xsdk_exfat_create(&o->volume->fs, o->path, directory, &node);
	if (status != EXFAT_OK)
		return status_of(status);
	const uint16_t extra = o->attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
	if (extra) {
		node.attributes |= extra;
		status = xsdk_exfat_update(&o->volume->fs, &node);
		if (status != EXFAT_OK)
			return status_of(status);
	}
	File *const f = file_new(o->volume, &node, o->path, o->path_length);
	if (!f)
		return STATUS_INSUFFICIENT_RESOURCES;
	if (!directory && (o->options & FILE_DELETE_ON_CLOSE))
		f->delete_on_close = true;
	const NTSTATUS attached = attach(o, f, FILE_CREATED);
	if (!NT_SUCCESS(attached))
		file_release_unused(f);
	return attached;
}

// The device itself, opened with an empty name: XAM's volume queries come through such a handle.
static NTSTATUS open_volume(Open *o)
{
	if (o->disposition != FILE_OPEN && o->disposition != FILE_OPEN_IF)
		return STATUS_ACCESS_DENIED;
	if (o->options & FILE_DIRECTORY_FILE)
		return STATUS_NOT_A_DIRECTORY;
	if (o->options & FILE_DELETE_ON_CLOSE)
		return STATUS_CANNOT_DELETE;
	o->fo->flags |= FILE_OBJECT_NO_BUFFERING;
	SHARE_ACCESS volume_share;
	IoSetShareAccess(o->access, 0, o->fo, &volume_share);
	o->fo->fs_context = NULL;
	o->fo->fs_context2 = NULL;
	list_append(&o->volume->files, &o->fo->fs_link);
	return complete(o->irp, STATUS_SUCCESS, FILE_OPENED);
}

// The path under the volume: the related file's path and the name, or the name without its leading backslash, with
// trailing separators taken off.
static NTSTATUS build_path(Open *o, const ANSI_STRING *name)
{
	const File *const related = o->fo->related_file_object ? o->fo->related_file_object->fs_context : NULL;
	uint32_t length = 0;
	if (o->fo->related_file_object) {
		if (!related || !is_directory(related))
			return STATUS_INVALID_PARAMETER;
		if (name->length && is_separator(name->pointer[0]))
			return STATUS_OBJECT_NAME_INVALID;
		memcpy(o->path, related->path, related->path_length);
		length = related->path_length;
		if (length && name->length)
			o->path[length++] = '\\';
	} else if (name->length) {
		if (!is_separator(name->pointer[0]))
			return STATUS_OBJECT_NAME_INVALID;
	}
	uint32_t start = 0;
	while (start < name->length && is_separator(name->pointer[start]))
		start++;
	uint32_t end = name->length;
	while (end > start && is_separator(name->pointer[end - 1]))
		end--;
	o->trailing_separator = end < name->length && end > start;
	if (length + (end - start) >= PATH_BYTES)
		return STATUS_OBJECT_NAME_INVALID;
	for (uint32_t i = start; i < end; i++)
		o->path[length++] = name->pointer[i] == '/' ? '\\' : name->pointer[i];
	o->path[length] = 0;
	o->path_length = length;
	return STATUS_SUCCESS;
}

static NTSTATUS create_locked(Open *o)
{
	static const ANSI_STRING no_name = { 0, 0, "" };
	const IO_STACK_LOCATION *const sp = o->irp->current_stack_location;
	const ANSI_STRING *const name = sp->create.remaining_name ? sp->create.remaining_name : &no_name;
	// Should the kernel send CLOSE for an open that failed, there is nothing to unlink.
	list_init(&o->fo->fs_link);
	o->fo->fs_context = NULL;
	o->fo->fs_context2 = NULL;
	if (!o->volume->mounted)
		return STATUS_VOLUME_DISMOUNTED;
	const NTSTATUS path_status = build_path(o, name);
	if (!NT_SUCCESS(path_status))
		return path_status;
	TRACE_FILE(o->path, "create: \"%s\" access 0x%08X disposition %u options 0x%06X share %u flags 0x%02X", o->path,
		(unsigned)o->access, (unsigned)o->disposition, (unsigned)o->options, (unsigned)o->share, sp->flags);
	if (!o->fo->related_file_object && name->length == 0)
		return open_volume(o);
	ExfatNode node;
	ExfatStatus status = xsdk_exfat_lookup(&o->volume->fs, o->path, &node);
	if (status == EXFAT_OK) {
		if (o->target_directory)
			return open_target_directory(o, true);
		return open_existing(o, &node, o->path, o->path_length);
	}
	if (status != EXFAT_NOT_FOUND && status != EXFAT_NOT_DIRECTORY && status != EXFAT_NAME)
		return status_of(status);
	// The last component is missing, or something before it: the parent decides which.
	uint32_t parent_length;
	uint32_t leaf;
	split_leaf(o->path, o->path_length, &parent_length, &leaf);
	char parent_path[PATH_BYTES];
	memcpy(parent_path, o->path, parent_length);
	parent_path[parent_length] = 0;
	ExfatNode parent;
	const ExfatStatus parent_status = xsdk_exfat_lookup(&o->volume->fs, parent_path, &parent);
	if (parent_status != EXFAT_OK || !(parent.attributes & FILE_ATTRIBUTE_DIRECTORY))
		return parent_status == EXFAT_NAME ? STATUS_OBJECT_NAME_INVALID : STATUS_OBJECT_PATH_NOT_FOUND;
	if (status != EXFAT_NOT_FOUND)
		return status_of(status);
	if (o->target_directory)
		return open_target_directory(o, false);
	return create_new(o);
}

static NTSTATUS dispatch_create(DEVICE_OBJECT *device, IRP *irp)
{
	const IO_STACK_LOCATION *const sp = irp->current_stack_location;
	Open o = {
		.volume = volume_of(device),
		.irp = irp,
		.fo = sp->file_object,
		.access = sp->create.desired_access,
		.disposition = sp->create.options >> 24,
		.options = sp->create.options & 0xFFFFFF,
		.share = sp->create.share_access,
		.attributes = sp->create.file_attributes,
		.target_directory = (sp->flags & SL_OPEN_TARGET_DIRECTORY) != 0,
	};
	lock_module();
	touch(device);
	const NTSTATUS status = create_locked(&o);
	unlock_module();
	// A success completed the request itself, with its information.
	if (NT_SUCCESS(status))
		return status;
	TRACE_FILE(o.path, "create: \"%s\" failed 0x%08X", o.path, (unsigned)status);
	return complete(irp, status, 0);
}

// IRP_MJ_CLEANUP (FAT 0x800a28a0) and IRP_MJ_CLOSE (0x800a4d90).

static void sync_volume(Volume *v)
{
	if (!v->mounted || !(v->fs.dirty_blocks || v->fs.dirtied))
		return;
	const ExfatStatus status = xsdk_exfat_sync(&v->fs);
	if (status != EXFAT_OK)
		log_line("%s: sync failed: %s", v->device_name, xsdk_exfat_status_name(status));
}

static NTSTATUS dispatch_cleanup(DEVICE_OBJECT *device, IRP *irp)
{
	Volume *const v = volume_of(device);
	FILE_OBJECT *const fo = irp->current_stack_location->file_object;
	lock_module();
	touch(device);
	File *const f = fo->fs_context;
	TRACE_FILE(f ? f->path : "", "cleanup: \"%s\"", f ? f->path : "(volume)");
	if (v->rw_bytes >= (uint64_t)SUMMARY_BYTES_MIB << 20) {
		log_line(
			"%s: %u reads and writes (%u with unaligned buffers), %u MiB: %u ms in the module, %u ms in %u "
			"device IRPs",
			v->device_name, (unsigned)v->rw_requests, (unsigned)v->unaligned_requests, (unsigned)(v->rw_bytes >> 20),
			(unsigned)(v->dispatch_ticks / TIME_BASE_TICKS_PER_MS), (unsigned)(v->io_ticks / TIME_BASE_TICKS_PER_MS),
			(unsigned)v->irps);
		v->rw_requests = 0;
		v->unaligned_requests = 0;
		v->rw_bytes = 0;
		v->dispatch_ticks = 0;
		v->io_ticks = 0;
		v->irps = 0;
	}
	if (f) {
		f->opens--;
		if (f->opens == 0 && f->delete_on_close && !f->deleted && !f->orphan) {
			const ExfatStatus status = xsdk_exfat_remove(&v->fs, &f->node);
			if (status == EXFAT_OK)
				f->deleted = true;
			else
				log_line("%s: deleting \"%s\" at its close failed: %s", v->device_name, f->path,
					xsdk_exfat_status_name(status));
		}
		if (!f->orphan)
			IoRemoveShareAccess(fo, &f->share);
		if (f->opens == 0 && !f->orphan)
			sync_volume(v);
	}
	search_free(fo);
	fo->flags |= FILE_OBJECT_CLOSED;
	unlock_module();
	return complete(irp, STATUS_SUCCESS, 0);
}

static NTSTATUS dispatch_close(DEVICE_OBJECT *device, IRP *irp)
{
	FILE_OBJECT *const fo = irp->current_stack_location->file_object;
	lock_module();
	touch(device);
	File *const f = fo->fs_context;
	TRACE_FILE(f ? f->path : "", "close: \"%s\"", f ? f->path : "(volume)");
	list_remove(&fo->fs_link);
	if (f && --f->references == 0)
		f->volume = NULL;
	fo->fs_context = NULL;
	unlock_module();
	return complete(irp, STATUS_SUCCESS, 0);
}

// IRP_MJ_READ and IRP_MJ_WRITE: FAT 0x800a4890.

static NTSTATUS read_write_locked(Volume *v, IRP *irp, uint32_t *information)
{
	const IO_STACK_LOCATION *const sp = irp->current_stack_location;
	FILE_OBJECT *const fo = sp->file_object;
	File *const f = fo->fs_context;
	const bool write = sp->major_function == IRP_MJ_WRITE;
	NTSTATUS status = file_object_usable(v, fo);
	if (!NT_SUCCESS(status))
		return status;
	if (sp->read.length == 0)
		return STATUS_SUCCESS;
	// shortcut: the device opened as a volume is not read or written raw, as FAT passes those down; no caller needs
	// it yet, add the pass-down when one does.
	if (!f || is_directory(f))
		return STATUS_INVALID_DEVICE_REQUEST;
	uint64_t offset = sp->read.byte_offset;
	TRACE_FILE(f->path, "%s: \"%s\" %u bytes at %u", write ? "write" : "read", f->path, (unsigned)sp->read.length,
		(unsigned)offset);
	if (write) {
		// -1 appends (kernel_seam.md, section 4).
		if (offset == UINT64_MAX)
			offset = f->node.size_bytes;
		const ExfatStatus result = xsdk_exfat_write(&v->fs, &f->node, offset, irp->user_buffer, sp->read.length);
		if (result != EXFAT_OK)
			return status_of(result);
		*information = sp->read.length;
	} else {
		if (sp->read.key)
			return STATUS_INVALID_PARAMETER;
		if (offset >= f->node.size_bytes)
			return STATUS_END_OF_FILE;
		const ExfatStatus result =
			xsdk_exfat_read(&v->fs, &f->node, offset, irp->user_buffer, sp->read.length, information);
		if (result != EXFAT_OK)
			return status_of(result);
	}
	// Where the next call without an offset goes, as NT's file systems move it for synchronous files.
	if (fo->flags & FILE_OBJECT_SYNCHRONOUS)
		fo->current_byte_offset = offset + *information;
	return STATUS_SUCCESS;
}

static NTSTATUS dispatch_read_write(DEVICE_OBJECT *device, IRP *irp)
{
	uint32_t information = 0;
	Volume *const v = volume_of(device);
	lock_module();
	touch(device);
	const uint32_t started = time_base();
	const NTSTATUS status = read_write_locked(v, irp, &information);
	v->dispatch_ticks += time_base() - started;
	v->rw_requests++;
	v->rw_bytes += NT_SUCCESS(status) ? information : 0;
	v->unaligned_requests += (address_of(irp->user_buffer) & 4095) != 0;
	unlock_module();
	return complete(irp, status, NT_SUCCESS(status) ? information : 0);
}

// IRP_MJ_QUERY_INFORMATION: FAT 0x800a2fa0 answers internal, position and network-open, nothing else.

static NTSTATUS query_information_locked(Volume *v, IRP *irp, uint32_t *information)
{
	const IO_STACK_LOCATION *const sp = irp->current_stack_location;
	FILE_OBJECT *const fo = sp->file_object;
	const File *const f = fo->fs_context;
	const NTSTATUS status = file_object_usable(v, fo);
	if (!NT_SUCCESS(status))
		return status;
	const uint32_t class = sp->information.information_class;
	TRACE_FILE(f ? f->path : "", "query_information: \"%s\" class %u", f ? f->path : "(volume)", (unsigned)class);
	if (!f && class != FILE_POSITION_INFORMATION_CLASS)
		return STATUS_INVALID_PARAMETER;
	uint8_t *const out = irp->user_buffer;
	switch (class) {
	case FILE_INTERNAL_INFORMATION_CLASS: {
		const uint32_t words[2] = { address_of(v), address_of(f) };
		memcpy(out, words, sizeof words);
		*information = FILE_INTERNAL_INFORMATION_BYTES;
		return STATUS_SUCCESS;
	}
	case FILE_POSITION_INFORMATION_CLASS: {
		const uint64_t position = fo->current_byte_offset;
		memcpy(out, &position, sizeof position);
		*information = FILE_POSITION_INFORMATION_BYTES;
		return STATUS_SUCCESS;
	}
	case FILE_NETWORK_OPEN_INFORMATION_CLASS: {
		const FILE_NETWORK_OPEN_INFORMATION network = {
			.creation_time = f->node.created,
			.last_access_time = f->node.accessed,
			.last_write_time = f->node.modified,
			.change_time = f->node.modified,
			.allocation_size = allocation_of(v, &f->node),
			.end_of_file = f->node.size_bytes,
			.attributes = attributes_of(&f->node),
		};
		memcpy(out, &network, sizeof network);
		*information = sizeof network;
		return STATUS_SUCCESS;
	}
	default:
		return STATUS_INVALID_PARAMETER;
	}
}

static NTSTATUS dispatch_query_information(DEVICE_OBJECT *device, IRP *irp)
{
	uint32_t information = 0;
	lock_module();
	touch(device);
	const NTSTATUS status = query_information_locked(volume_of(device), irp, &information);
	unlock_module();
	return complete(irp, status, NT_SUCCESS(status) ? information : 0);
}

// IRP_MJ_SET_INFORMATION: FAT 0x800a3bf0.

static bool directory_empty(Volume *v, const ExfatNode *directory, bool *empty)
{
	ExfatDirectory listing;
	ExfatNode entry;
	if (xsdk_exfat_open_directory(&v->fs, directory, &listing) != EXFAT_OK)
		return false;
	const ExfatStatus status = xsdk_exfat_read_directory(&v->fs, &listing, &entry);
	if (status != EXFAT_OK && status != EXFAT_NOT_FOUND)
		return false;
	*empty = status == EXFAT_NOT_FOUND;
	return true;
}

static NTSTATUS set_basic(Volume *v, File *f, const FILE_BASIC_INFORMATION *basic)
{
	if (is_root(f))
		return STATUS_ACCESS_DENIED;
	// A time or attributes given as 0 stay as they were (include/xsdk/kernel.h, FILE_BASIC_INFORMATION); NORMAL alone
	// clears the attributes.
	if (basic->creation_time)
		f->node.created = basic->creation_time;
	if (basic->last_access_time)
		f->node.accessed = basic->last_access_time;
	if (basic->last_write_time)
		f->node.modified = basic->last_write_time;
	else if (basic->change_time)
		f->node.modified = basic->change_time;
	if (basic->attributes) {
		const uint16_t kept = f->node.attributes & FILE_ATTRIBUTE_DIRECTORY;
		f->node.attributes = kept | (basic->attributes & (FILE_ATTRIBUTE_VALID & ~FILE_ATTRIBUTE_DIRECTORY));
	}
	return status_of(xsdk_exfat_update(&v->fs, &f->node));
}

static NTSTATUS set_rename(Volume *v, File *f, const FILE_RENAME_INFORMATION *rename, const FILE_OBJECT *target_fo)
{
	if (is_root(f))
		return STATUS_ACCESS_DENIED;
	if (rename->name.length == 0)
		return STATUS_OBJECT_NAME_INVALID;
	// The new name's last component; its directory is the one the kernel opened, else the file's own.
	uint32_t end = rename->name.length;
	while (end && is_separator(rename->name.pointer[end - 1]))
		end--;
	uint32_t start = end;
	while (start && !is_separator(rename->name.pointer[start - 1]))
		start--;
	const uint32_t leaf_length = end - start;
	if (leaf_length == 0 || leaf_length > EXFAT_NAME_MAX)
		return STATUS_OBJECT_NAME_INVALID;
	const char *directory_path;
	uint32_t directory_length;
	if (target_fo) {
		const File *const target = target_fo->fs_context;
		if (!target || target->volume != v || !is_directory(target))
			return STATUS_INVALID_PARAMETER;
		directory_path = target->path;
		directory_length = target->path_length;
	} else {
		uint32_t leaf;
		split_leaf(f->path, f->path_length, &directory_length, &leaf);
		directory_path = f->path;
	}
	if (directory_length + 1 + leaf_length >= PATH_BYTES)
		return STATUS_OBJECT_NAME_INVALID;
	char new_path[PATH_BYTES];
	uint32_t length = 0;
	memcpy(new_path, directory_path, directory_length);
	length = directory_length;
	if (length)
		new_path[length++] = '\\';
	memcpy(new_path + length, rename->name.pointer + start, leaf_length);
	length += leaf_length;
	new_path[length] = 0;
	if (is_directory(f) && has_open_children(f))
		return STATUS_ACCESS_DENIED;
	ExfatNode existing;
	ExfatStatus status = xsdk_exfat_lookup(&v->fs, new_path, &existing);
	if (status == EXFAT_OK && !same_entry(&existing, &f->node)) {
		if (!rename->replace_if_exists)
			return STATUS_OBJECT_NAME_COLLISION;
		if ((existing.attributes & FILE_ATTRIBUTE_DIRECTORY) || file_find(v, &existing))
			return STATUS_ACCESS_DENIED;
		status = xsdk_exfat_remove(&v->fs, &existing);
		if (status != EXFAT_OK)
			return status_of(status);
	} else if (status != EXFAT_OK && status != EXFAT_NOT_FOUND) {
		return status_of(status);
	}
	status = xsdk_exfat_rename(&v->fs, &f->node, new_path);
	if (status != EXFAT_OK)
		return status_of(status);
	memcpy(f->path, new_path, length + 1);
	f->path_length = length;
	return STATUS_SUCCESS;
}

static NTSTATUS set_disposition(Volume *v, File *f, bool delete)
{
	if (is_root(f))
		return STATUS_ACCESS_DENIED;
	if (delete) {
		if (f->node.attributes & FILE_ATTRIBUTE_READONLY)
			return STATUS_CANNOT_DELETE;
		if (is_directory(f)) {
			bool empty;
			if (!directory_empty(v, &f->node, &empty))
				return STATUS_DISK_CORRUPT_ERROR;
			if (!empty)
				return STATUS_DIRECTORY_NOT_EMPTY;
		}
	}
	f->delete_on_close = delete;
	return STATUS_SUCCESS;
}

static NTSTATUS set_information_locked(Volume *v, IRP *irp)
{
	const IO_STACK_LOCATION *const sp = irp->current_stack_location;
	FILE_OBJECT *const fo = sp->file_object;
	File *const f = fo->fs_context;
	const NTSTATUS status = file_object_usable(v, fo);
	if (!NT_SUCCESS(status))
		return status;
	const uint32_t class = sp->information.information_class;
	TRACE_FILE(f ? f->path : "", "set_information: \"%s\" class %u", f ? f->path : "(volume)", (unsigned)class);
	const void *const in = irp->user_buffer;
	if (class == FILE_POSITION_INFORMATION_CLASS) {
		// Only an asynchronous file's reaches a file system; the kernel moves a synchronous one's itself.
		uint64_t position;
		memcpy(&position, in, sizeof position);
		fo->current_byte_offset = position;
		return STATUS_SUCCESS;
	}
	if (!f)
		return STATUS_INVALID_PARAMETER;
	switch (class) {
	case FILE_BASIC_INFORMATION_CLASS:
		return set_basic(v, f, in);
	case FILE_RENAME_INFORMATION_CLASS:
		return set_rename(v, f, in, sp->information.target_file_object);
	case FILE_DISPOSITION_INFORMATION_CLASS:
		return set_disposition(v, f, ((const FILE_DISPOSITION_INFORMATION *)in)->delete_file != 0);
	case FILE_ALLOCATION_INFORMATION_CLASS:
	case FILE_END_OF_FILE_INFORMATION_CLASS: {
		if (is_directory(f))
			return STATUS_INVALID_PARAMETER;
		uint64_t size;
		memcpy(&size, in, sizeof size);
		// An allocation is only taken as far as it cuts the file; nothing is reserved ahead.
		if (class == FILE_ALLOCATION_INFORMATION_CLASS && size >= f->node.size_bytes)
			return STATUS_SUCCESS;
		return status_of(xsdk_exfat_resize(&v->fs, &f->node, size));
	}
	default:
		return STATUS_INVALID_PARAMETER;
	}
}

static NTSTATUS dispatch_set_information(DEVICE_OBJECT *device, IRP *irp)
{
	lock_module();
	touch(device);
	const NTSTATUS status = set_information_locked(volume_of(device), irp);
	unlock_module();
	return complete(irp, status, 0);
}

// IRP_MJ_QUERY_VOLUME_INFORMATION: FAT 0x800a2d18 answers classes 1, 3, 4 and 5.

static NTSTATUS query_volume_locked(Volume *v, IRP *irp, uint32_t *information)
{
	const IO_STACK_LOCATION *const sp = irp->current_stack_location;
	const NTSTATUS status = file_object_usable(v, sp->file_object);
	if (!NT_SUCCESS(status))
		return status;
	const uint32_t class = sp->information.information_class;
	const uint32_t length = sp->information.length;
	uint8_t *const out = irp->user_buffer;
	TRACE("query_volume_information: class %u length %u", (unsigned)class, (unsigned)length);
	switch (class) {
	case FILE_FS_VOLUME_INFORMATION_CLASS: {
		if (length < FS_VOLUME_INFORMATION_BYTES)
			return STATUS_BUFFER_TOO_SMALL;
		memset(out, 0, FS_VOLUME_INFORMATION_BYTES);
		const uint32_t label_length = (uint32_t)strlen(v->fs.label);
		memcpy(out + 8, &v->fs.serial, 4);
		memcpy(out + 12, &label_length, 4);
		const uint32_t room = length - FS_VOLUME_INFORMATION_BYTES;
		const uint32_t copied = label_length < room ? label_length : room;
		memcpy(out + FS_VOLUME_INFORMATION_BYTES, v->fs.label, copied);
		*information = FS_VOLUME_INFORMATION_BYTES + copied;
		return copied < label_length ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
	}
	case FILE_FS_SIZE_INFORMATION_CLASS: {
		if (length < FS_SIZE_INFORMATION_BYTES)
			return STATUS_BUFFER_TOO_SMALL;
		const FILE_FS_SIZE_INFORMATION size = {
			.total_allocation_units = v->fs.cluster_count,
			.available_allocation_units = v->fs.free_clusters,
			.sectors_per_allocation_unit = 1u << (v->fs.cluster_shift - v->fs.sector_shift),
			.bytes_per_sector = v->sector_bytes,
		};
		memcpy(out, &size, sizeof size);
		*information = FS_SIZE_INFORMATION_BYTES;
		return STATUS_SUCCESS;
	}
	case FILE_FS_DEVICE_INFORMATION_CLASS: {
		if (length < FS_DEVICE_INFORMATION_BYTES)
			return STATUS_BUFFER_TOO_SMALL;
		const uint32_t words[2] = { v->device->device_type, 0 };
		memcpy(out, words, sizeof words);
		*information = FS_DEVICE_INFORMATION_BYTES;
		return STATUS_SUCCESS;
	}
	case FILE_FS_ATTRIBUTE_INFORMATION_CLASS: {
		if (length < FS_ATTRIBUTE_INFORMATION_BYTES)
			return STATUS_BUFFER_TOO_SMALL;
		const uint32_t name_length = sizeof FILE_SYSTEM_NAME - 1;
		const uint32_t words[3] = { 0, EXFAT_NAME_MAX, name_length };
		memcpy(out, words, sizeof words);
		const uint32_t room = length - FS_ATTRIBUTE_INFORMATION_BYTES;
		const uint32_t copied = name_length < room ? name_length : room;
		memcpy(out + FS_ATTRIBUTE_INFORMATION_BYTES, FILE_SYSTEM_NAME, copied);
		*information = FS_ATTRIBUTE_INFORMATION_BYTES + copied;
		return copied < name_length ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
	}
	default:
		return STATUS_INVALID_PARAMETER;
	}
}

static NTSTATUS dispatch_query_volume_information(DEVICE_OBJECT *device, IRP *irp)
{
	uint32_t information = 0;
	lock_module();
	touch(device);
	const NTSTATUS status = query_volume_locked(volume_of(device), irp, &information);
	unlock_module();
	return complete(irp, status, NT_SUCCESS(status) ? information : 0);
}

// IRP_MJ_DIRECTORY_CONTROL: FAT 0x800a2a88, one entry a call.

// NT's * and ? against a name, both up-cased through the volume's table.
static bool name_matches(
	const Volume *v, const char *pattern, uint32_t pattern_length, const char *name, uint32_t name_length)
{
	uint32_t p = 0;
	uint32_t n = 0;
	uint32_t star_p = UINT32_MAX;
	uint32_t star_n = 0;
	while (n < name_length) {
		if (p < pattern_length && pattern[p] == '*') {
			star_p = p++;
			star_n = n;
		} else if (p < pattern_length &&
			(pattern[p] == '?' ||
				xsdk_exfat_upcase(&v->fs, (uint8_t)pattern[p]) == xsdk_exfat_upcase(&v->fs, (uint8_t)name[n]))) {
			p++;
			n++;
		} else if (star_p != UINT32_MAX) {
			p = star_p + 1;
			n = ++star_n;
		} else {
			return false;
		}
	}
	while (p < pattern_length && pattern[p] == '*')
		p++;
	return p == pattern_length;
}

static NTSTATUS directory_control_locked(Volume *v, IRP *irp, uint32_t *information)
{
	const IO_STACK_LOCATION *const sp = irp->current_stack_location;
	FILE_OBJECT *const fo = sp->file_object;
	File *const f = fo->fs_context;
	NTSTATUS status = file_object_usable(v, fo);
	if (!NT_SUCCESS(status))
		return status;
	if (!f || !is_directory(f))
		return STATUS_INVALID_PARAMETER;
	const uint32_t length = sp->query_directory.length;
	if (length < sizeof(FILE_DIRECTORY_INFORMATION))
		return STATUS_BUFFER_TOO_SMALL;
	Search *s = fo->fs_context2;
	const bool restart = (sp->flags & SL_RESTART_SCAN) != 0;
	if (!s) {
		s = search_new();
		if (!s)
			return STATUS_INSUFFICIENT_RESOURCES;
		fo->fs_context2 = s;
		s->first_query = true;
	}
	if (!s->opened || restart) {
		const ExfatStatus opened = xsdk_exfat_open_directory(&v->fs, &f->node, &s->listing);
		if (opened != EXFAT_OK)
			return status_of(opened);
		s->opened = true;
		s->index = 0;
		const ANSI_STRING *const pattern = sp->query_directory.file_name;
		s->pattern_length = 0;
		if (pattern && pattern->length && pattern->length <= EXFAT_NAME_MAX) {
			memcpy(s->pattern, pattern->pointer, pattern->length);
			s->pattern_length = pattern->length;
		}
	}
	TRACE("directory_control: \"%s\" pattern \"%.*s\"%s", f->path, (int)s->pattern_length, s->pattern,
		restart ? " restart" : "");
	const bool first_query = s->first_query;
	s->first_query = false;
	for (;;) {
		ExfatNode entry;
		const ExfatStatus read = xsdk_exfat_read_directory(&v->fs, &s->listing, &entry);
		if (read == EXFAT_NOT_FOUND)
			return first_query ? STATUS_NO_SUCH_FILE : STATUS_NO_MORE_FILES;
		if (read != EXFAT_OK)
			return status_of(read);
		const uint32_t name_length = (uint32_t)strlen(entry.name);
		if (s->pattern_length && !name_matches(v, s->pattern, s->pattern_length, entry.name, name_length))
			continue;
		FILE_DIRECTORY_INFORMATION *const out = irp->user_buffer;
		const uint32_t room = (length - sizeof *out) & ~3u;
		const uint32_t copied = name_length < room ? name_length : room;
		out->next_entry_offset = 0;
		out->file_index = s->index++;
		out->creation_time = entry.created;
		out->last_access_time = entry.accessed;
		out->last_write_time = entry.modified;
		out->change_time = entry.modified;
		out->end_of_file = entry.size_bytes;
		out->allocation_size = allocation_of(v, &entry);
		out->attributes = attributes_of(&entry);
		out->file_name_length = name_length;
		memcpy(out->file_name, entry.name, copied);
		*information = sizeof *out + copied;
		return copied < name_length ? STATUS_BUFFER_OVERFLOW : STATUS_SUCCESS;
	}
}

static NTSTATUS dispatch_directory_control(DEVICE_OBJECT *device, IRP *irp)
{
	uint32_t information = 0;
	lock_module();
	touch(device);
	const NTSTATUS status = directory_control_locked(volume_of(device), irp, &information);
	unlock_module();
	return complete(irp, status, NT_SUCCESS(status) || status == STATUS_BUFFER_OVERFLOW ? information : 0);
}

// IRP_MJ_FLUSH_BUFFERS: what is cached goes to the device, then the flush goes down to the device as FAT sends it
// (kernel_seam.md, 0x800a2750), so a drive's own write cache is flushed too; a stick's handler is the kernel's stub.
static NTSTATUS dispatch_flush_buffers(DEVICE_OBJECT *device, IRP *irp)
{
	Volume *const v = volume_of(device);
	IO_STACK_LOCATION *const sp = irp->current_stack_location;
	lock_module();
	touch(device);
	NTSTATUS status = file_object_usable(v, sp->file_object);
	if (NT_SUCCESS(status)) {
		TRACE("flush_buffers");
		status = v->fs.dirty_blocks || v->fs.dirtied ? status_of(xsdk_exfat_sync(&v->fs)) : STATUS_SUCCESS;
	}
	unlock_module();
	if (!NT_SUCCESS(status))
		return complete(irp, status, 0);
	IO_STACK_LOCATION *const next = sp - 1;
	memcpy(next, sp, offsetof(IO_STACK_LOCATION, completion_routine));
	next->control = 0;
	return IoCallDriver(v->device, irp);
}

// IRP_MJ_DEVICE_CONTROL goes down to the device as FAT sends it (0x800a2750): the parameters copied to the next
// stack location, no completion routine.
static NTSTATUS dispatch_device_control(DEVICE_OBJECT *device, IRP *irp)
{
	Volume *const v = volume_of(device);
	IO_STACK_LOCATION *const sp = irp->current_stack_location;
	TRACE("device_control: 0x%08X", (unsigned)sp->device_io_control.io_control_code);
	IO_STACK_LOCATION *const next = sp - 1;
	memcpy(next, sp, offsetof(IO_STACK_LOCATION, completion_routine));
	next->control = 0;
	return IoCallDriver(v->device, irp);
}

// Mounting and dismounting (kernel_seam.md, sections 3, 5 and 6).

static void dismount_volume(DEVICE_OBJECT *volume_device);
static void delete_device(DEVICE_OBJECT *volume_device);
static ANSI_STRING ansi(const char *text);

static const DRIVER_OBJECT driver = {
	.start_io = NULL,
	.delete_device = delete_device,
	.dismount_volume = dismount_volume,
	.major_function = {
		[IRP_MJ_CREATE] = dispatch_create,
		[IRP_MJ_CLOSE] = dispatch_close,
		[IRP_MJ_READ] = dispatch_read_write,
		[IRP_MJ_WRITE] = dispatch_read_write,
		[IRP_MJ_QUERY_INFORMATION] = dispatch_query_information,
		[IRP_MJ_SET_INFORMATION] = dispatch_set_information,
		[IRP_MJ_FLUSH_BUFFERS] = dispatch_flush_buffers,
		[IRP_MJ_QUERY_VOLUME_INFORMATION] = dispatch_query_volume_information,
		[IRP_MJ_DIRECTORY_CONTROL] = dispatch_directory_control,
		[IRP_MJ_DEVICE_CONTROL] = dispatch_device_control,
		[IRP_MJ_CLEANUP] = dispatch_cleanup,
	},
};

// FatDismountVolume's protocol (0x800a2650): the device forgets the volume under the I/O manager's lock, every open
// file object is marked so its next request fails STATUS_VOLUME_DISMOUNTED, and the volume device is deleted.
static void dismount_locked(Volume *v, const char *why)
{
	if (!v->mounted)
		return;
	uint32_t open_files = 0;
	const uint32_t state = IoAcquireDeviceObjectLock();
	if (v->device->mounted_or_self_device == v->volume_device)
		v->device->mounted_or_self_device = NULL;
	IoReleaseDeviceObjectLock(state);
	v->mounted = false;
	while (v->files.flink != address_of(&v->files)) {
		FILE_OBJECT *const fo = file_object_of((KERNEL_LIST_ENTRY *)(uintptr_t)v->files.flink);
		fo->flags |= FILE_OBJECT_DISMOUNTED;
		search_free(fo);
		list_remove(&fo->fs_link);
		open_files++;
	}
	for (unsigned i = 0; i < FILES_MAX; i++) {
		if (files[i].volume == v)
			files[i].orphan = true;
	}
	log_line("%s: dismounted (%s), %u file objects were open", v->device_name, why, (unsigned)open_files);
	v->pending_deletes++;
	IoDeleteDevice(v->volume_device);
	v->volume_device = NULL;
}

static void dismount_volume(DEVICE_OBJECT *volume_device)
{
	Volume *const v = volume_of(volume_device);
	uint64_t now;
	KeQuerySystemTime(&now);
	lock_module();
	if (v == &volumes[PARTITION] && now < xam_shield_until)
		log_line("%s: dismount declined, xam's look after the announcement", v->device_name);
	else if (v->mounted && v->volume_device == volume_device) {
		sync_volume(v);
		dismount_locked(v, "asked by the kernel");
	}
	unlock_module();
}

// Once the volume device's last reference is gone: FatDeleteDevice's two dereferences (0x800a25d0).
static void delete_device(DEVICE_OBJECT *volume_device)
{
	Extension *const e = volume_device->device_extension;
	TRACE("%s: volume device deleted", e->volume->device_name);
	e->volume->pending_deletes--;
	ObDereferenceObject(address_of(e->device));
	ObDereferenceObject(address_of(volume_device));
}

static void log_mounted(const Volume *v)
{
	log_line(
		"%s: mounted exFAT \"%s\" serial %08X: %u sectors of %u from sector %u, %u KB clusters, %u of %u "
		"free, volume device %08X",
		v->device_name, v->fs.label, (unsigned)v->fs.serial, (unsigned)v->fs.device.sectors, (unsigned)v->sector_bytes,
		(unsigned)v->fs.device.first_sector, (unsigned)(1u << v->fs.cluster_shift >> 10), (unsigned)v->fs.free_clusters,
		(unsigned)v->fs.cluster_count, address_of(v->volume_device));
}

// xam lists the drive from one look: as it loads it enters the content partition itself (xam 2.0.17559.0, 0x8167ed90,
// when the hardware has a drive) and marks it unformatted when NtQueryFullAttributesFile on the partition's root
// fails (the look 0x8167e5b8, the test 0x816e0540). xam loads at 0x800613b0, before this module's hook at 0x800613dc
// (kernel_seam.md, section 7), so that look never finds our volume, and the dash's storage settings called the exFAT
// drive unformatted (results/2026-11-15-milestone-78-step-1, runs 9 to 11). Announcing the partition again once our
// volume is on it, as the kernel announces a stick, makes xam look again (its worker 0x8167eb60). When xam drops a
// drive it listed as formatted it dismounts the drive first (0x8167e760), so dismount_volume declines the partition's
// dismounts for the next two seconds, and the look finds our volume.
static void announce_partition(void)
{
	partition_record.name = ansi(volumes[PARTITION].device_name);
	partition_record.type = STORAGE_TYPE_HDD;
	uint64_t now;
	KeQuerySystemTime(&now);
	xam_shield_until = now + XAM_SHIELD_100NS;
	*(volatile uint16_t *)(uintptr_t)XAM_DEVICE_ID_COUNTER = STORAGE_ID_HDD;
	log_line("%s: announced to xam again, type %u, as device %u", volumes[PARTITION].device_name, STORAGE_TYPE_HDD,
		STORAGE_ID_HDD);
	((void (*)(StorageRecord *, uint32_t))(uintptr_t)KERNEL_ANNOUNCE_STORAGE)(&partition_record, STORAGE_ARRIVAL);
}

static void try_mount(Volume *v)
{
	const ExfatDevice found = {
		.read = device_read,
		.write = device_write,
		.now = device_now,
		.context = v,
		.sector_bytes = v->sector_bytes,
		.first_sector = 0,
		.sectors = v->sectors,
	};
	ExfatDevice device = found;
	// Under the device's mount lock from the first read, as the kernel's own mounts run (IopMountDevice, section 3):
	// a title formatting the device holds it (tests/exfat_format.c), and sector 0 is read once it has let go.
	KeWaitForSingleObject(&v->device->mount_lock, 0, KERNEL_MODE, 0, NULL);
	ExfatStatus status = xsdk_exfat_locate(&device);
	if (status != EXFAT_OK) {
		// FATX and FAT32 sticks are the kernel's; anything else stays unmounted and is said once.
		if (status != EXFAT_NOT_EXFAT)
			log_line("%s: not mounted: %s", v->device_name, xsdk_exfat_status_name(status));
		v->refused = true;
		KeSetEvent(&v->device->mount_lock, 0, 0);
		return;
	}
	DEVICE_OBJECT *mounted = v->device->mounted_or_self_device;
	if (mounted && mounted->driver_object == (const DRIVER_OBJECT *)(uintptr_t)KERNEL_RAW_DRIVER) {
		const NTSTATUS dismounted = IoDismountVolume(v->device);
		log_line("%s: raw volume dismounted: 0x%08X", v->device_name, (unsigned)dismounted);
		mounted = v->device->mounted_or_self_device;
	}
	if (mounted) {
		KeSetEvent(&v->device->mount_lock, 0, 0);
		return;
	}
	status = xsdk_exfat_mount(&v->fs, &device);
	if (status != EXFAT_OK) {
		log_line("%s: exFAT volume refused: %s", v->device_name, xsdk_exfat_status_name(status));
		v->refused = true;
		KeSetEvent(&v->device->mount_lock, 0, 0);
		return;
	}
	DEVICE_OBJECT *volume_device = NULL;
	const NTSTATUS created =
		IoCreateDevice(&driver, sizeof(Extension), NULL, DEVICE_TYPE_VOLUME, 0, (void **)&volume_device);
	if (!NT_SUCCESS(created)) {
		log_line("%s: IoCreateDevice failed: 0x%08X", v->device_name, (unsigned)created);
		v->refused = true;
		KeSetEvent(&v->device->mount_lock, 0, 0);
		return;
	}
	volume_device->stack_size += v->device->stack_size;
	if (v->device->alignment_requirement > volume_device->alignment_requirement)
		volume_device->alignment_requirement = v->device->alignment_requirement;
	if (v->device->flags & DEVICE_FLAG_LOCK_PAGES)
		volume_device->flags |= DEVICE_FLAG_LOCK_PAGES;
	volume_device->sector_size = v->device->sector_size;
	ObReferenceObject(v->device);
	Extension *const e = volume_device->device_extension;
	e->device = v->device;
	e->volume = v;
	list_init(&v->files);
	v->volume_device = volume_device;
	v->mounted = true;
	volume_device->flags &= ~(uint32_t)DEVICE_FLAG_NOT_READY;
	v->device->mounted_or_self_device = volume_device;
	KeSetEvent(&v->device->mount_lock, 0, 0);
	// The log lives on the partition: a mount of it is a fresh or a formatted volume, so the thread remakes the log's
	// folder and logs again (results/2026-11-15-milestone-78-step-1, the format's boot logged nothing).
	if (v == &volumes[PARTITION]) {
		log_ready = false;
		announce_pending = true;
	}
	// Before the log's folder exists the line would be dropped; the thread logs every mounted volume once it does.
	if (log_ready)
		log_mounted(v);
}

static bool reference_device(Volume *v)
{
	const ANSI_STRING name = ansi(v->device_name);
	uint32_t object = 0;
	if (!NT_SUCCESS(ObReferenceObjectByName(&name, 0, IoDeviceObjectType, NULL, &object)) || !object)
		return false;
	v->device = (DEVICE_OBJECT *)(uintptr_t)object;
	if (v->device->device_type != v->device_type) {
		log_line("%s: device type %u, not %u; left alone", v->device_name, (unsigned)v->device->device_type,
			(unsigned)v->device_type);
		ObDereferenceObject(object);
		v->device = NULL;
		return false;
	}
	return true;
}

static void release_device(Volume *v)
{
	if (v->device)
		ObDereferenceObject(address_of(v->device));
	v->device = NULL;
	v->refused = false;
}

static void poll(Volume *v)
{
	// A poll that waited for the lock while a stream held it slowed the stream's device IRPs after it: 256 MiB at
	// 9 MiB/s against 15 raw, whatever the poll then did, 14 with a poll every 10 s and 15 skipping a taken lock
	// (results/2026-11-14-milestone-77-step-3/speed9 to speed15). So the poll leaves a busy module alone.
	KeEnterCriticalRegion();
	if (!RtlTryEnterCriticalSection(&lock)) {
		KeLeaveCriticalRegion();
		return;
	}
	if (v->mounted) {
		// A busy volume is left alone until a quiet poll, or SYNC_POLLS_MAX of them: a metadata write in the middle
		// of a stream costs the stick some 25 ms (results/2026-11-14-milestone-77-step-3/speed5). A pulled stick
		// fails the stream's own IRPs meanwhile.
		const bool quiet = v->requests == v->requests_seen;
		v->requests_seen = v->requests;
		if (quiet || ++v->busy_polls >= SYNC_POLLS_MAX) {
			v->busy_polls = 0;
			if (!media_present(v)) {
				dismount_locked(v, "media gone");
				release_device(v);
			} else {
				sync_volume(v);
			}
		}
	} else if (v->device || reference_device(v)) {
		const DEVICE_OBJECT *const mounted = v->device->mounted_or_self_device;
		const bool raw = mounted && mounted->driver_object == (const DRIVER_OBJECT *)(uintptr_t)KERNEL_RAW_DRIVER;
		if (!media_present(v))
			release_device(v);
		else if (mounted && !raw)
			v->refused = false;
		// A device nothing has mounted is read every poll: a dismount, the formatter's among them, leaves it so.
		else if (!mounted || !v->refused)
			try_mount(v);
	}
	unlock_module();
}

// The thread.

static ANSI_STRING ansi(const char *text)
{
	const ANSI_STRING string = { (uint16_t)strlen(text), (uint16_t)(strlen(text) + 1), text };
	return string;
}

static bool file_exists(const char *path)
{
	const ANSI_STRING name = ansi(path);
	const OBJECT_ATTRIBUTES attributes = { .name = &name };
	FILE_NETWORK_OPEN_INFORMATION information;
	return NT_SUCCESS(NtQueryFullAttributesFile(&attributes, &information));
}

// Whether a flag file is on the partition. Looked up in the volume itself under a lock the thread only tries, as a
// look through the kernel would wait for the module's lock and be handed it while the thread is starved of its
// hardware thread: xam's UI pump runs above a plugin's base priority through a pending task, and run 33's profile
// creation waited 36 s on a lock the thread owned but could not run to release
// (results/2026-11-15-milestone-78-step-1). A busy module keeps the flag as it was. A partition that is not ours,
// FATX, is asked through the kernel, whose own driver serves it.
static bool flag_present(const char *name, const char *path, bool while_busy)
{
	Volume *const v = &volumes[PARTITION];
	if (!v->mounted)
		return file_exists(path);
	KeEnterCriticalRegion();
	if (!RtlTryEnterCriticalSection(&lock)) {
		KeLeaveCriticalRegion();
		return while_busy;
	}
	ExfatNode node;
	const bool present = v->mounted && xsdk_exfat_lookup(&v->fs, name, &node) == EXFAT_OK;
	unlock_module();
	return present;
}

// Deleted at its close, as src/path.c's unlink does, so the next load doesn't unload at once.
static NTSTATUS remove_file(const char *path)
{
	const ANSI_STRING name = ansi(path);
	const OBJECT_ATTRIBUTES attributes = { .name = &name };
	HANDLE file;
	IO_STATUS_BLOCK io_status;
	NTSTATUS status = NtCreateFile(&file, DELETE | SYNCHRONIZE, &attributes, &io_status, NULL, FILE_ATTRIBUTE_NORMAL, 0,
		FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
	if (!NT_SUCCESS(status))
		return status;
	const FILE_DISPOSITION_INFORMATION disposition = { .delete_file = 1 };
	status =
		NtSetInformationFile(file, &io_status, &disposition, sizeof disposition, FILE_DISPOSITION_INFORMATION_CLASS);
	const NTSTATUS close_status = NtClose(file);
	return NT_SUCCESS(status) ? close_status : status;
}

// Opens or makes a folder; false where its volume cannot be written, as the partition is before its mount.
static bool make_folder(const char *path)
{
	const ANSI_STRING name = ansi(path);
	const OBJECT_ATTRIBUTES attributes = { .name = &name };
	HANDLE folder;
	IO_STATUS_BLOCK io_status;
	if (!NT_SUCCESS(
			NtCreateFile(&folder, GENERIC_READ | SYNCHRONIZE, &attributes, &io_status, NULL, FILE_ATTRIBUTE_NORMAL,
				FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_DIRECTORY_FILE)))
		return false;
	(void)NtClose(folder);
	return true;
}

// The log's folder and the one above it: a freshly formatted partition has neither.
static bool make_log_folder(void)
{
	return make_folder(XSDK_FOLDER) && make_folder(LOG_FOLDER);
}

static void sleep_ms(uint32_t milliseconds)
{
	const int64_t interval = -(int64_t)milliseconds * 10000;
	(void)KeDelayExecutionThread(KERNEL_MODE, 0, &interval);
}

// The boot timeline (milestone 78 step 1): what the partition's device holds, each change, timed from the module's
// start.

enum { STATE_ABSENT, STATE_NONE, STATE_RAW, STATE_OTHER, STATE_OURS, STATE_GATE };
static const char *const state_names[] = { "no device", "no volume", "the kernel's raw volume",
	"another file system's volume", "our volume", "the executable set, the gate open" };
typedef struct {
	uint32_t ms;
	uint8_t state;
} Sample;
static Sample timeline[TIMELINE_MAX];
static unsigned timeline_count;
static uint64_t started;
// The partition's device, referenced for the sampling; its volume takes a reference of its own.
static DEVICE_OBJECT *partition;

static void record(uint8_t state)
{
	if (timeline_count && timeline[timeline_count - 1].state == state)
		return;
	uint64_t now;
	KeQuerySystemTime(&now);
	if (timeline_count < TIMELINE_MAX)
		timeline[timeline_count++] = (Sample){ (uint32_t)((now - started) / 10000), state };
}

// Reads the device object alone: no IRP, no open, nothing the boot's storage pass can collide with.
static void sample_partition(void)
{
	if (!partition) {
		const ANSI_STRING name = ansi(device_names[PARTITION]);
		uint32_t object = 0;
		if (!NT_SUCCESS(ObReferenceObjectByName(&name, 0, IoDeviceObjectType, NULL, &object)) || !object) {
			record(STATE_ABSENT);
			return;
		}
		partition = (DEVICE_OBJECT *)(uintptr_t)object;
	}
	const DEVICE_OBJECT *const mounted = partition->mounted_or_self_device;
	if (!mounted)
		record(STATE_NONE);
	else if (mounted->driver_object == (const DRIVER_OBJECT *)(uintptr_t)KERNEL_RAW_DRIVER)
		record(STATE_RAW);
	else if (volumes && mounted == volumes[PARTITION].volume_device)
		record(STATE_OURS);
	else
		record(STATE_OTHER);
}

static void log_timeline(void)
{
	for (unsigned i = 0; i < timeline_count; i++) {
		if (timeline[i].state == STATE_GATE)
			log_line("boot: +%u ms, %s", timeline[i].ms, state_names[STATE_GATE]);
		else
			log_line("boot: +%u ms, %s: %s", timeline[i].ms, device_names[PARTITION], state_names[timeline[i].state]);
	}
	if (timeline_count == TIMELINE_MAX)
		log_line("boot: the timeline is full");
}

void plugin_main(HANDLE module)
{
	KeQuerySystemTime(&started);
	RtlInitializeCriticalSection(&lock);
	RtlInitializeCriticalSection(&queue_lock);
	// This thread is handed the module's lock whenever it waited for it behind the dash's I/O (RtlEnterCriticalSection
	// wakes one waiter, the owner before it runs). Starved of its hardware thread by xam's UI task at a higher
	// priority, it kept the lock 36 s and the dash's profile creation timed out
	// (results/2026-11-15-milestone-78-step-1, run 33). It runs at the highest base priority the kernel allows a system
	// thread, which the kernel clamps the request to.
	const uintptr_t self = *(const volatile uint32_t *)(pcr() + PCR_THREAD);
	const int32_t previous = KeSetBasePriorityThread((void *)self, MODULE_PRIORITY);
	log_line("exfat: thread priority %d -> %d", (int)previous, (int)KeQueryBasePriorityThread((void *)self));
	volumes = MmAllocatePhysicalMemoryEx(0, sizeof(Volume) * VOLUMES_MAX, PAGE_READWRITE, 0, 0xFFFFFFFF, 4096);
	if (volumes) {
		memset(volumes, 0, sizeof(Volume) * VOLUMES_MAX);
		for (unsigned i = 0; i < VOLUMES_MAX; i++) {
			volumes[i].device_name = device_names[i];
			volumes[i].device_type = device_types[i];
			list_init(&volumes[i].files);
		}
	}
	// Loaded at boot by the kernel's init thread, the module starts a second before xam, whose storage thread then
	// dismounts and remounts each storage device as it registers it, the HDD's content partition and the sticks alike,
	// and the dash loads only once that pass is over. A log line or a stick's mount during that pass hangs the boot
	// (results/2026-11-14-milestone-77-step-4/README.md, "xam's storage start-up"), so the sticks and the log wait for
	// the executable, set when xam loads the dash or DashLaunch's lhelper.xex. An empty content partition gets one
	// mount attempt at once, ahead of the pass, where a FATX drive's kernel volume already is; the timeline samples
	// what the pass does to it (results/2026-11-15-milestone-78-step-1, run 9).
	bool tried = false;
	while (!*XexExecutableModuleHandle) {
		sample_partition();
		if (volumes && !tried && partition && !partition->mounted_or_self_device) {
			poll(&volumes[PARTITION]);
			tried = true;
		}
		sleep_ms(POLL_MS);
	}
	record(STATE_GATE);
	xsdk_plugin_log_open(LOG_PATH);
	if (!volumes) {
		(void)make_log_folder();
		log_line("exfat: no memory for the volumes, unloading");
		write_queued();
		return;
	}
	// The log lives on the partition, so on an exFAT one it can be written only once the poll has mounted it: the
	// loaded line, the boot timeline and the mounted volumes' lines wait for its folder.
	while (!flag_present(UNLOAD_NAME, UNLOAD_PATH, false)) {
		sample_partition();
		if (!log_ready && make_log_folder()) {
			log_ready = true;
			const LDR_DATA_TABLE_ENTRY *const entry = (const LDR_DATA_TABLE_ENTRY *)(uintptr_t)module;
			log_line("exfat: loaded, kernel %u.%u.%u.%u, image 0x%08X-0x%08X, %u KB a volume, %u volumes",
				(unsigned)XboxKrnlVersion->major, (unsigned)XboxKrnlVersion->minor, (unsigned)XboxKrnlVersion->build,
				(unsigned)XboxKrnlVersion->qfe, (unsigned)entry->image_base,
				(unsigned)(entry->image_base + entry->image_size), (unsigned)(sizeof(Volume) >> 10), VOLUMES_MAX);
			log_timeline();
			for (unsigned i = 0; i < VOLUMES_MAX; i++) {
				if (volumes[i].mounted)
					log_mounted(&volumes[i]);
			}
		}
		if (announce_pending) {
			announce_pending = false;
			announce_partition();
		}
		if (log_ready)
			write_queued();
		const bool tracing = flag_present(TRACE_NAME, TRACE_PATH, trace);
		if (tracing != trace) {
			trace = tracing;
			log_line("exfat: trace %s", tracing ? "on" : "off");
		}
		for (unsigned i = 0; i < VOLUMES_MAX; i++)
			poll(&volumes[i]);
		sleep_ms(POLL_MS);
	}
	write_queued();
	lock_module();
	for (unsigned i = 0; i < VOLUMES_MAX; i++) {
		Volume *const v = &volumes[i];
		if (v->mounted) {
			sync_volume(v);
			dismount_locked(v, "unloading");
		}
		release_device(v);
	}
	unlock_module();
	// Unloading with a volume device still referenced would leave the kernel a driver object in freed memory.
	for (unsigned waited = 0;; waited += POLL_MS) {
		uint32_t pending = 0;
		lock_module();
		for (unsigned i = 0; i < VOLUMES_MAX; i++)
			pending += volumes[i].pending_deletes;
		unlock_module();
		if (!pending)
			break;
		if (waited % 10000 == 0)
			log_line("exfat: %u volume devices still referenced by open handles; waiting to unload", (unsigned)pending);
		write_queued();
		sleep_ms(POLL_MS);
	}
	if (partition)
		ObDereferenceObject(address_of(partition));
	const NTSTATUS removed = remove_file(UNLOAD_PATH);
	log_line("exfat: exfat_unload.txt found, removed: 0x%08X; unloading", (unsigned)removed);
	write_queued();
}
