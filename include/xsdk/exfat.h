#ifndef XSDK_EXFAT_H
#define XSDK_EXFAT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// An exFAT volume over two sector callbacks (docs/scope/77-exfat.md, step 2): plain C with no kernel calls, built into
// the console's module and into the PC harness (tests/exfat_host/). The layouts are Microsoft's exFAT specification
// (2019): the boot sector (section 3.1), the FAT (4), the allocation bitmap (7.1), the up-case table (7.2) and the
// directory entries (6, 7.4 to 7.7). Names are the console's 8-bit characters, Latin-1 in UTF-16; an entry whose name
// holds other characters lists with '?' in their place and can't be opened.

enum {
	EXFAT_SECTOR_BYTES_MAX = 4096,
	// The metadata cache: 4 KB blocks of FAT, bitmap and directory sectors, 256 KB in all.
	EXFAT_CACHE_BLOCK_BYTES = 4096,
	EXFAT_CACHE_BLOCKS = 64,
	// The compressed up-case table is 5836 bytes as Windows and mkfs.exfat write it; a longer one is refused.
	EXFAT_UPCASE_BYTES_MAX = 8192,
	EXFAT_NAME_MAX = 255,
	EXFAT_LABEL_MAX = 11,
};

typedef enum {
	EXFAT_OK = 0,
	// The device's read or write failed.
	EXFAT_IO,
	// Sector 0 is neither an exFAT boot sector nor an MBR with an exFAT partition.
	EXFAT_NOT_EXFAT,
	// A checksum, a cluster number or a chain is wrong.
	EXFAT_CORRUPT,
	// A valid volume the core doesn't take: sectors over 4 KB, two FATs, an up-case table over 8 KB.
	EXFAT_UNSUPPORTED,
	EXFAT_NOT_FOUND,
	EXFAT_EXISTS,
	EXFAT_NOT_DIRECTORY,
	EXFAT_IS_DIRECTORY,
	EXFAT_NOT_EMPTY,
	// No free cluster.
	EXFAT_FULL,
	// A component that is empty, over 255 characters, "." or "..", or holds a character exFAT forbids or Latin-1
	// lacks; also the root where a named entry is needed.
	EXFAT_NAME,
} ExfatStatus;

typedef struct {
	// Whole sectors on the device, numbered from the device's start; 0 on success, anything else on a failure.
	int (*read)(void *context, uint64_t sector, uint32_t count, void *buffer);
	int (*write)(void *context, uint64_t sector, uint32_t count, const void *buffer);
	// The time now as a FILETIME (100 ns units since 1601, UTC) for the entries the core writes; NULL writes 1980.
	uint64_t (*now)(void *context);
	void *context;
	uint32_t sector_bytes;
	// The volume's sectors on the device: its first and its count. xsdk_exfat_locate fills them from sector 0;
	// xsdk_exfat_format takes the whole device, first_sector 0.
	uint64_t first_sector;
	uint64_t sectors;
} ExfatDevice;

// A file or directory as its entry set describes it, and what an open file holds between calls. The core writes the
// set back through it, so one node per file at a time. The root has no set: parent_cluster is 0.
typedef struct {
	char name[EXFAT_NAME_MAX + 1];
	// exFAT's FileAttributes: read-only 0x01, hidden 0x02, system 0x04, directory 0x10, archive 0x20.
	uint16_t attributes;
	// DataLength, and ValidDataLength: what has been written; bytes beyond it read as zeros.
	uint64_t size_bytes;
	uint64_t valid_bytes;
	// FILETIMEs, UTC.
	uint64_t created;
	uint64_t modified;
	uint64_t accessed;
	// The core's: where the data is, where the set is in its directory, and the last chain step taken.
	uint32_t first_cluster;
	bool contiguous;
	uint32_t parent_cluster;
	bool parent_contiguous;
	uint64_t set_offset;
	uint8_t secondary_count;
	uint32_t walk_index;
	uint32_t walk_cluster;
} ExfatNode;

typedef struct {
	// The directory itself.
	ExfatNode node;
	// The next entry to read.
	uint64_t offset;
} ExfatDirectory;

typedef struct {
	// Byte offset in the volume / EXFAT_CACHE_BLOCK_BYTES; UINT64_MAX when empty.
	uint64_t block;
	uint32_t used;
	bool dirty;
} ExfatCacheSlot;

typedef struct {
	ExfatDevice device;
	uint32_t sector_shift;
	uint32_t cluster_shift;
	uint64_t volume_bytes;
	uint64_t fat_offset;
	uint64_t fat_bytes;
	uint64_t heap_offset;
	uint32_t cluster_count;
	uint32_t root_cluster;
	uint64_t root_bytes;
	uint32_t serial;
	// VolumeDirty as the boot sector has it now, and whether this mount set it; only then does a sync clear it.
	bool dirty;
	bool dirtied;
	uint32_t bitmap_cluster;
	uint64_t bitmap_bytes;
	uint32_t free_clusters;
	// Where the next search for a free cluster starts.
	uint32_t next_free;
	char label[EXFAT_LABEL_MAX + 1];
	uint32_t upcase_words;
	uint16_t upcase[EXFAT_UPCASE_BYTES_MAX / 2];
	uint32_t tick;
	uint32_t dirty_blocks;
	ExfatCacheSlot slots[EXFAT_CACHE_BLOCKS];
	uint8_t blocks[EXFAT_CACHE_BLOCKS][EXFAT_CACHE_BLOCK_BYTES];
	uint8_t bounce[EXFAT_SECTOR_BYTES_MAX];
} ExfatVolume;

// Reads sector 0 of a device whose sectors field holds its whole count: an exFAT boot sector there makes the volume
// the device; an MBR with a type 07 partition makes it that partition. Fills first_sector and sectors.
ExfatStatus xsdk_exfat_locate(ExfatDevice *device);
ExfatStatus xsdk_exfat_mount(ExfatVolume *volume, const ExfatDevice *device);
// Writes what is cached, the FAT first, the bitmap next, directories last, then clears VolumeDirty.
ExfatStatus xsdk_exfat_sync(ExfatVolume *volume);
ExfatStatus xsdk_exfat_unmount(ExfatVolume *volume);
const char *xsdk_exfat_status_name(ExfatStatus status);

// Components are separated by '\' or '/'; "" or "\" is the root.
ExfatStatus xsdk_exfat_lookup(ExfatVolume *volume, const char *path, ExfatNode *node);
// A character up-cased through the volume's table, as names compare.
uint16_t xsdk_exfat_upcase(const ExfatVolume *volume, uint16_t c);
ExfatStatus xsdk_exfat_open_directory(const ExfatVolume *volume, const ExfatNode *directory, ExfatDirectory *listing);
// The next entry; EXFAT_NOT_FOUND after the last.
ExfatStatus xsdk_exfat_read_directory(ExfatVolume *volume, ExfatDirectory *listing, ExfatNode *entry);
// Fewer bytes than asked past the end; none at or beyond it.
ExfatStatus xsdk_exfat_read(
	ExfatVolume *volume, ExfatNode *file, uint64_t offset, void *buffer, uint32_t bytes, uint32_t *read_bytes);
// Grows the file as needed; a gap before what was valid is zeroed.
ExfatStatus xsdk_exfat_write(ExfatVolume *volume, ExfatNode *file, uint64_t offset, const void *buffer, uint32_t bytes);
// Shrinks or grows; grown bytes read as zeros without being written.
ExfatStatus xsdk_exfat_resize(ExfatVolume *volume, ExfatNode *file, uint64_t size_bytes);
// Writes the node's attributes and times back to its set.
ExfatStatus xsdk_exfat_update(ExfatVolume *volume, const ExfatNode *node);
ExfatStatus xsdk_exfat_create(ExfatVolume *volume, const char *path, bool directory, ExfatNode *node);
// A file, or an empty directory.
ExfatStatus xsdk_exfat_remove(ExfatVolume *volume, const ExfatNode *node);
// To a new name, in the same directory or another; the node then describes the new set.
ExfatStatus xsdk_exfat_rename(ExfatVolume *volume, ExfatNode *node, const char *new_path);

// Writes an MBR with one type 07 partition from sector 2048 to the last whole megabyte, and on it what Windows writes
// for a volume of that size: its cluster size, its FAT and heap offsets, one FAT, the bitmap, the up-case table and a
// root directory holding the label (up to 11 characters, "" for none), the bitmap and the up-case entries.
ExfatStatus xsdk_exfat_format(const ExfatDevice *device, const char *label, uint32_t serial);

#ifdef __cplusplus
}
#endif

#endif
