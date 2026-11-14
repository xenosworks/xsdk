#ifndef XSDK_EXFAT_KERNEL_IO_H
#define XSDK_EXFAT_KERNEL_IO_H

#include <stdint.h>

#include "xsdk/kernel.h"

// The I/O manager's objects as kernel 2.0.17559.0 lays them out, read from its disassembly (kernel_seam.md, section
// 1; the offsets there are the authority, the asserts below hold the structs to them). Only the fields the module
// touches are named; the rest keep their offsets as padding. The kernel keeps its 64-bit fields 4-byte aligned, so
// those are marked so; read them by value, never by address.
#define KERNEL_UINT64 uint64_t __attribute__((packed, aligned(4)))

typedef struct {
	uint32_t flink;
	uint32_t blink;
} KERNEL_LIST_ENTRY;

// DISPATCHER_HEADER and its wait list, as NT has them; a synchronization event's SignalState is 1 when free.
typedef struct {
	uint8_t type;
	uint8_t absolute;
	uint8_t size;
	uint8_t inserted;
	int32_t signal_state;
	KERNEL_LIST_ENTRY wait_list;
} KEVENT;

typedef struct DEVICE_OBJECT DEVICE_OBJECT;
typedef struct DRIVER_OBJECT DRIVER_OBJECT;
typedef struct IRP IRP;
typedef struct FILE_OBJECT FILE_OBJECT;

struct DEVICE_OBJECT {
	uint16_t type;
	uint16_t size;
	int32_t reference_count;
	const DRIVER_OBJECT *driver_object;
	// NULL on a mountable device until a file system claims it, then its volume device.
	DEVICE_OBJECT *mounted_or_self_device;
	uint32_t unknown_10;
	uint32_t flags;
	void *device_extension;
	uint8_t device_type;
	uint8_t unknown_1d;
	uint8_t stack_size;
	uint8_t delete_pending;
	uint32_t sector_size;
	uint32_t alignment_requirement;
	uint32_t device_queue[4];
	// A synchronization event the kernel holds while it mounts a volume on the device.
	KEVENT mount_lock;
	uint32_t unknown_48[2];
};

typedef NTSTATUS (*DRIVER_DISPATCH)(DEVICE_OBJECT *device, IRP *irp);

struct DRIVER_OBJECT {
	void (*start_io)(DEVICE_OBJECT *device, IRP *irp);
	void (*delete_device)(DEVICE_OBJECT *device);
	void (*dismount_volume)(DEVICE_OBJECT *device);
	DRIVER_DISPATCH major_function[11];
};

typedef struct {
	NTSTATUS status;
	uint32_t information;
} KERNEL_IO_STATUS;

typedef struct {
	uint8_t major_function;
	uint8_t minor_function;
	uint8_t flags;
	uint8_t control;
	union {
		struct {
			uint32_t desired_access;
			// Disposition << 24 | the create options.
			uint32_t options;
			uint16_t file_attributes;
			uint16_t share_access;
			const ANSI_STRING *remaining_name;
		} create;
		struct {
			uint32_t length;
			uint32_t key;
			KERNEL_UINT64 byte_offset;
		} read;
		struct {
			uint32_t length;
			uint32_t information_class;
			// SET_INFORMATION's rename: the directory the new name is in, opened by the kernel with the stack
			// location's flag 4; NULL when the new name is relative.
			FILE_OBJECT *target_file_object;
		} information;
		struct {
			uint32_t length;
			// NULL or empty for every entry.
			const ANSI_STRING *file_name;
		} query_directory;
		struct {
			uint32_t output_buffer_length;
			void *input_buffer;
			uint32_t input_buffer_length;
			uint32_t io_control_code;
		} device_io_control;
	};
	DEVICE_OBJECT *device_object;
	FILE_OBJECT *file_object;
	void *completion_routine;
	void *context;
} IO_STACK_LOCATION;

struct IRP {
	uint16_t type;
	uint16_t size;
	uint32_t flags;
	KERNEL_LIST_ENTRY thread_list;
	KERNEL_IO_STATUS io_status;
	int8_t stack_count;
	int8_t current_location;
	uint8_t pending_returned;
	uint8_t cancel;
	// The caller's data for READ and WRITE, the information buffer for the query and set classes.
	void *user_buffer;
	void *user_iosb;
	void *user_event;
	uint64_t allocation_size;
	uint32_t apc[4];
	uint32_t unknown_40;
	void *thread;
	uint32_t unknown_48[2];
	IO_STACK_LOCATION *current_stack_location;
	FILE_OBJECT *original_file_object;
};

struct FILE_OBJECT {
	uint16_t type;
	uint8_t flags;
	// What IoSetShareAccess and IoCheckShareAccess leave: 0x20 read, 0x40 write, 0x10 delete, from the desired
	// access; NtReadFile and NtWriteFile check it.
	uint8_t access;
	DEVICE_OBJECT *device_object;
	void *fs_context;
	void *fs_context2;
	uint32_t unknown_10;
	// Where a synchronous file's next read or write goes when the caller gives no offset.
	KERNEL_UINT64 current_byte_offset;
	FILE_OBJECT *related_file_object;
	void *completion_port;
	uint32_t unknown_24;
	KEVENT lock;
	KEVENT event;
	uint32_t unknown_48[2];
	// The file system's: this module lists a volume's file objects through it.
	KERNEL_LIST_ENTRY fs_link;
	uint8_t process_type;
	uint8_t unknown_59[15];
};

// IoSetShareAccess, IoCheckShareAccess and IoRemoveShareAccess keep 7 counts of one byte each: opens, readers, writers,
// deleters, and the opens sharing read, write and delete (IoSetShareAccess 0x8006bbd0, IoCheckShareAccess 0x8006b9d0).
typedef struct {
	uint8_t counts[8];
} SHARE_ACCESS;

_Static_assert(sizeof(KEVENT) == 0x10, "DISPATCHER_HEADER and the wait list");
_Static_assert(sizeof(DEVICE_OBJECT) == 0x50, "kernel_seam.md, section 1");
_Static_assert(__builtin_offsetof(DEVICE_OBJECT, mounted_or_self_device) == 0x0C, "kernel_seam.md, section 1");
_Static_assert(__builtin_offsetof(DEVICE_OBJECT, flags) == 0x14, "kernel_seam.md, section 1");
_Static_assert(__builtin_offsetof(DEVICE_OBJECT, device_extension) == 0x18, "kernel_seam.md, section 1");
_Static_assert(__builtin_offsetof(DEVICE_OBJECT, sector_size) == 0x20, "kernel_seam.md, section 1");
_Static_assert(__builtin_offsetof(DEVICE_OBJECT, mount_lock) == 0x38, "kernel_seam.md, section 1");
_Static_assert(sizeof(DRIVER_OBJECT) == 0x38, "kernel_seam.md, section 1");
_Static_assert(sizeof(IO_STACK_LOCATION) == 36, "kernel_seam.md, section 1");
_Static_assert(__builtin_offsetof(IO_STACK_LOCATION, create.remaining_name) == 0x10, "kernel_seam.md, section 1");
_Static_assert(__builtin_offsetof(IO_STACK_LOCATION, read.byte_offset) == 0x0C, "kernel_seam.md, section 1");
_Static_assert(__builtin_offsetof(IO_STACK_LOCATION, information.target_file_object) == 0x0C,
	"NtSetInformationFile 0x8006d928 writes it at +0x0C");
_Static_assert(__builtin_offsetof(IO_STACK_LOCATION, device_object) == 0x14, "kernel_seam.md, section 1");
_Static_assert(__builtin_offsetof(IO_STACK_LOCATION, file_object) == 0x18, "kernel_seam.md, section 1");
_Static_assert(sizeof(IRP) == 0x58, "kernel_seam.md, section 1");
_Static_assert(__builtin_offsetof(IRP, io_status) == 0x10, "kernel_seam.md, section 1");
_Static_assert(__builtin_offsetof(IRP, user_buffer) == 0x1C, "kernel_seam.md, section 1");
_Static_assert(__builtin_offsetof(IRP, allocation_size) == 0x28, "kernel_seam.md, section 1");
_Static_assert(__builtin_offsetof(IRP, current_stack_location) == 0x50, "kernel_seam.md, section 1");
_Static_assert(sizeof(FILE_OBJECT) == 0x68, "kernel_seam.md, section 1");
_Static_assert(__builtin_offsetof(FILE_OBJECT, current_byte_offset) == 0x14, "kernel_seam.md, section 1");
_Static_assert(__builtin_offsetof(FILE_OBJECT, related_file_object) == 0x1C, "kernel_seam.md, section 1");
_Static_assert(__builtin_offsetof(FILE_OBJECT, fs_link) == 0x50, "kernel_seam.md, section 1");

enum {
	IRP_MJ_CREATE = 0,
	IRP_MJ_CLOSE = 1,
	IRP_MJ_READ = 2,
	IRP_MJ_WRITE = 3,
	IRP_MJ_QUERY_INFORMATION = 4,
	IRP_MJ_SET_INFORMATION = 5,
	IRP_MJ_FLUSH_BUFFERS = 6,
	IRP_MJ_QUERY_VOLUME_INFORMATION = 7,
	IRP_MJ_DIRECTORY_CONTROL = 8,
	IRP_MJ_DEVICE_CONTROL = 9,
	IRP_MJ_CLEANUP = 10,
	IRP_MJ_COUNT = 11,

	// The CREATE stack location's flag for a rename's target: open the last component's directory and say whether
	// the component exists (NT's SL_OPEN_TARGET_DIRECTORY; NtSetInformationFile 0x8006d928, FAT's create 0x800a51c8).
	SL_OPEN_TARGET_DIRECTORY = 0x04,
	// NtQueryDirectoryFile's restart flag in the stack location (0x8006cfa8).
	SL_RESTART_SCAN = 0x01,

	// DEVICE_OBJECT.flags
	DEVICE_FLAG_NEVER_MOUNT = 0x01,
	DEVICE_FLAG_LOCK_PAGES = 0x04,
	DEVICE_FLAG_NOT_READY = 0x10,
	DEVICE_TYPE_MASS_STORAGE = 45,
	DEVICE_TYPE_VOLUME = 8,

	// FILE_OBJECT.flags
	FILE_OBJECT_SYNCHRONOUS = 0x01,
	FILE_OBJECT_NO_BUFFERING = 0x04,
	FILE_OBJECT_CLOSED = 0x10,
	FILE_OBJECT_CLEANED_UP = 0x20,
	FILE_OBJECT_DISMOUNTED = 0x80,

	// IRP.io_status.information after CREATE (NT's values; FAT 0x800a51c8 gives the same).
	FILE_SUPERSEDED = 0,
	FILE_OPENED = 1,
	FILE_CREATED = 2,
	FILE_OVERWRITTEN = 3,
	FILE_EXISTS = 4,
	FILE_DOES_NOT_EXIST = 5,

	// Dispositions and create options NtCreateFile packs into the stack location's options; the public ones are in
	// kernel.h.
	FILE_SUPERSEDE = 0,
	FILE_DELETE_ON_CLOSE = 0x00001000,
	// Access rights (NT's values); the generic rights reach a file system turned into these.
	FILE_READ_DATA = 0x0001,
	FILE_WRITE_DATA = 0x0002,
	FILE_APPEND_DATA = 0x0004,
	FILE_READ_ATTRIBUTES = 0x0080,
	FILE_ATTRIBUTE_HIDDEN = 0x0002,
	FILE_ATTRIBUTE_SYSTEM = 0x0004,
	FILE_ATTRIBUTE_ARCHIVE = 0x0020,
	// The attributes a file system keeps (FAT masks with it).
	FILE_ATTRIBUTE_VALID = 0x0037,

	// The information classes a file system sees (xenia:src/xenia/kernel/info/file.h:19-55); the kernel answers
	// position, mode and alignment itself for synchronous files (NtQueryInformationFile 0x8006d630).
	FILE_INTERNAL_INFORMATION_CLASS = 6,
	FILE_POSITION_INFORMATION_CLASS = 14,
	FILE_ALLOCATION_INFORMATION_CLASS = 19,
	// Volume information classes (xenia:src/xenia/kernel/info/volume.h:17-22).
	FILE_FS_VOLUME_INFORMATION_CLASS = 1,
	FILE_FS_DEVICE_INFORMATION_CLASS = 4,
	FILE_FS_ATTRIBUTE_INFORMATION_CLASS = 5,

	// What the USB mass-storage class driver answers (kernel_seam.md, section 2).
	IOCTL_DISK_GET_DRIVE_GEOMETRY = 0x70000,
	IOCTL_DISK_GET_PARTITION_INFO = 0x74004,
};

#endif
