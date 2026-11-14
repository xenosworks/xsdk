#ifndef XSDK_EXFAT_PRIVATE_H
#define XSDK_EXFAT_PRIVATE_H

#include <stdbool.h>
#include <stdint.h>

// The on-disk layout exfat.c and exfat_format.c share: Microsoft's exFAT specification (2019), sections 3.1 (the boot
// sector), 6.2 to 6.4 (entry types), 7.1 to 7.7 (the critical entries) and the checksums of 3.1.2, 6.4.3, 7.2.3 and
// 7.6.4. Every field is little-endian.

enum {
	EXFAT_ENTRY_BYTES = 32,
	EXFAT_NAME_ENTRY_CHARS = 15,
	EXFAT_BOOT_SECTORS = 12,
	EXFAT_FIRST_CLUSTER = 2,
	// The FAT's end-of-chain mark and its bad-cluster mark.
	EXFAT_CLUSTER_END = 0xFFFFFFFF,
	EXFAT_CLUSTER_BAD = 0xFFFFFFF7,

	// The boot sector.
	EXFAT_BOOT_NAME = 3,
	EXFAT_BOOT_PARTITION_OFFSET = 64,
	EXFAT_BOOT_VOLUME_LENGTH = 72,
	EXFAT_BOOT_FAT_OFFSET = 80,
	EXFAT_BOOT_FAT_LENGTH = 84,
	EXFAT_BOOT_HEAP_OFFSET = 88,
	EXFAT_BOOT_CLUSTER_COUNT = 92,
	EXFAT_BOOT_ROOT_CLUSTER = 96,
	EXFAT_BOOT_SERIAL = 100,
	EXFAT_BOOT_REVISION = 104,
	EXFAT_BOOT_FLAGS = 106,
	EXFAT_BOOT_SECTOR_SHIFT = 108,
	EXFAT_BOOT_CLUSTER_SHIFT = 109,
	EXFAT_BOOT_FAT_COUNT = 110,
	EXFAT_BOOT_DRIVE_SELECT = 111,
	EXFAT_BOOT_PERCENT_IN_USE = 112,
	EXFAT_BOOT_SIGNATURE = 510,
	EXFAT_FLAG_DIRTY = 2,

	// Entry types; a cleared bit 7 marks an entry unused.
	EXFAT_TYPE_END = 0x00,
	EXFAT_TYPE_IN_USE = 0x80,
	EXFAT_TYPE_BITMAP = 0x81,
	EXFAT_TYPE_UPCASE = 0x82,
	EXFAT_TYPE_LABEL = 0x83,
	EXFAT_TYPE_FILE = 0x85,
	EXFAT_TYPE_STREAM = 0xC0,
	EXFAT_TYPE_NAME = 0xC1,

	// The file entry.
	EXFAT_FILE_SECONDARY_COUNT = 1,
	EXFAT_FILE_CHECKSUM = 2,
	EXFAT_FILE_ATTRIBUTES = 4,
	EXFAT_FILE_CREATED = 8,
	EXFAT_FILE_MODIFIED = 12,
	EXFAT_FILE_ACCESSED = 16,
	EXFAT_FILE_CREATED_10MS = 20,
	EXFAT_FILE_MODIFIED_10MS = 21,
	EXFAT_FILE_CREATED_OFFSET = 22,
	EXFAT_FILE_MODIFIED_OFFSET = 23,
	EXFAT_FILE_ACCESSED_OFFSET = 24,
	EXFAT_ATTRIBUTE_DIRECTORY = 0x10,
	EXFAT_ATTRIBUTE_ARCHIVE = 0x20,

	// The stream extension entry.
	EXFAT_STREAM_FLAGS = 1,
	EXFAT_STREAM_NAME_LENGTH = 3,
	EXFAT_STREAM_NAME_HASH = 4,
	EXFAT_STREAM_VALID_LENGTH = 8,
	EXFAT_STREAM_FIRST_CLUSTER = 20,
	EXFAT_STREAM_DATA_LENGTH = 24,
	EXFAT_STREAM_ALLOCATION_POSSIBLE = 1,
	EXFAT_STREAM_NO_FAT_CHAIN = 2,

	// The name entry's characters, the bitmap and up-case entries' cluster and length, the label's count and text.
	EXFAT_NAME_CHARS = 2,
	EXFAT_ALLOCATION_FIRST_CLUSTER = 20,
	EXFAT_ALLOCATION_DATA_LENGTH = 24,
	EXFAT_UPCASE_CHECKSUM = 4,
	EXFAT_LABEL_COUNT = 1,
	EXFAT_LABEL_TEXT = 2,
};

static inline uint16_t exfat_le16(const uint8_t *bytes)
{
	return (uint16_t)(bytes[0] | bytes[1] << 8);
}

static inline uint32_t exfat_le32(const uint8_t *bytes)
{
	return bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static inline uint64_t exfat_le64(const uint8_t *bytes)
{
	return exfat_le32(bytes) | (uint64_t)exfat_le32(bytes + 4) << 32;
}

static inline void exfat_put16(uint8_t *bytes, uint16_t value)
{
	bytes[0] = (uint8_t)value;
	bytes[1] = (uint8_t)(value >> 8);
}

static inline void exfat_put32(uint8_t *bytes, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++)
		bytes[i] = (uint8_t)(value >> 8 * i);
}

static inline void exfat_put64(uint8_t *bytes, uint64_t value)
{
	exfat_put32(bytes, (uint32_t)value);
	exfat_put32(bytes + 4, (uint32_t)(value >> 32));
}

// The 32-bit checksum of the boot region (3.1.2) and of the up-case table (7.2.3): rotate right, then add each byte;
// the boot region leaves out VolumeFlags and PercentInUse, which change while mounted.
static inline uint32_t exfat_checksum32(uint32_t checksum, const uint8_t *bytes, uint32_t count, bool boot)
{
	for (uint32_t i = 0; i < count; i++) {
		if (boot && (i == EXFAT_BOOT_FLAGS || i == EXFAT_BOOT_FLAGS + 1 || i == EXFAT_BOOT_PERCENT_IN_USE))
			continue;
		checksum = (checksum >> 1 | checksum << 31) + bytes[i];
	}
	return checksum;
}

// The 16-bit checksum of an entry set (6.4.3), which leaves out its own field, and the name hash (7.6.4).
static inline uint16_t exfat_checksum16(uint16_t checksum, const uint8_t *bytes, uint32_t count, bool skip_checksum)
{
	for (uint32_t i = 0; i < count; i++) {
		if (skip_checksum && (i == EXFAT_FILE_CHECKSUM || i == EXFAT_FILE_CHECKSUM + 1))
			continue;
		checksum = (uint16_t)((checksum >> 1 | checksum << 15) + bytes[i]);
	}
	return checksum;
}

#endif
