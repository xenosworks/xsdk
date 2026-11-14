#include "xsdk/exfat.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "exfat_private.h"
#include "exfat_upcase.h"

// Formats a device the way Windows 11 formatted the 58680 MB stick (results/2026-11-14-milestone-77-step-1/recipe):
// an MBR whose one type 07 partition starts at 1 MB and ends 1 MB short of the last whole megabyte, a FAT at 1 MB into
// the partition sized up to a whole cluster, the cluster heap at the next megabyte, the bitmap from cluster 2, the
// up-case table after it and the root directory after that. Clusters are Windows's defaults for the volume's size.

enum {
	SCRATCH_BYTES = 65536,
	UPCASE_BYTES = EXFAT_UPCASE_TABLE_WORDS * 2,
	MBR_DISK_SIGNATURE = 440,
	MBR_PARTITION_TABLE = 446,
	MBR_TYPE_EXFAT = 0x07,
	// A boot region's sectors, main or backup.
	BOOT_REGION_SECTORS = EXFAT_BOOT_SECTORS,
	// Fewer clusters than this is a volume no stick has.
	CLUSTERS_MIN = 64,
};

static const uint64_t MB = 1 << 20;
static const uint64_t CLUSTER_4K_UP_TO = 256 * MB;
static const uint64_t CLUSTER_32K_UP_TO = 32 * 1024 * MB;

typedef struct {
	const ExfatDevice *device;
	uint32_t sector_shift;
	// The partition on the device, in sectors, and the volume's fields in its sectors.
	uint64_t partition_sector;
	uint64_t volume_sectors;
	uint32_t cluster_shift;
	uint32_t fat_sector;
	uint32_t fat_sectors;
	uint32_t heap_sector;
	uint32_t cluster_count;
	uint32_t bitmap_clusters;
	uint32_t upcase_clusters;
	uint32_t root_cluster;
	uint32_t serial;
} Layout;

static uint8_t scratch[SCRATCH_BYTES];

static uint32_t log2_of(uint64_t value)
{
	uint32_t shift = 0;
	while (((uint64_t)1 << shift) < value)
		shift++;
	return shift;
}

static uint64_t round_up(uint64_t value, uint64_t unit)
{
	return (value + unit - 1) / unit * unit;
}

static ExfatStatus plan(const ExfatDevice *device, uint32_t serial, Layout *l)
{
	const uint32_t sector_bytes = device->sector_bytes;
	if (sector_bytes < 512 || sector_bytes > EXFAT_SECTOR_BYTES_MAX || (sector_bytes & (sector_bytes - 1)))
		return EXFAT_UNSUPPORTED;
	memset(l, 0, sizeof(*l));
	l->device = device;
	l->serial = serial;
	l->sector_shift = log2_of(sector_bytes);
	const uint64_t mb_sectors = MB >> l->sector_shift;
	const uint64_t whole_mb = device->sectors / mb_sectors;
	if (whole_mb < 4)
		return EXFAT_UNSUPPORTED;
	l->partition_sector = mb_sectors;
	l->volume_sectors = (whole_mb - 2) * mb_sectors;
	const uint64_t volume_bytes = l->volume_sectors << l->sector_shift;
	uint32_t cluster_bytes_shift = volume_bytes <= CLUSTER_4K_UP_TO ? 12 : volume_bytes <= CLUSTER_32K_UP_TO ? 15 : 17;
	if (cluster_bytes_shift < l->sector_shift)
		cluster_bytes_shift = l->sector_shift;
	l->cluster_shift = cluster_bytes_shift - l->sector_shift;
	l->fat_sector = (uint32_t)mb_sectors;
	// The count from an upper bound, down to where the FAT it needs and the heap after it leave room for it.
	uint64_t count = (l->volume_sectors - l->fat_sector) >> l->cluster_shift;
	while (true) {
		const uint64_t fat_bytes = (count + 2) * 4;
		l->fat_sectors =
			(uint32_t)round_up(round_up(fat_bytes, sector_bytes) >> l->sector_shift, (uint64_t)1 << l->cluster_shift);
		l->heap_sector = (uint32_t)round_up(l->fat_sector + l->fat_sectors, mb_sectors);
		const uint64_t fits = (l->volume_sectors - l->heap_sector) >> l->cluster_shift;
		if (fits >= count)
			break;
		count = fits;
	}
	if (count > 0xFFFFFFF5)
		count = 0xFFFFFFF5;
	if (count < CLUSTERS_MIN)
		return EXFAT_UNSUPPORTED;
	l->cluster_count = (uint32_t)count;
	const uint64_t cluster_bytes = (uint64_t)1 << cluster_bytes_shift;
	l->bitmap_clusters = (uint32_t)(((count + 7) / 8 + cluster_bytes - 1) / cluster_bytes);
	l->upcase_clusters = (uint32_t)((UPCASE_BYTES + cluster_bytes - 1) / cluster_bytes);
	l->root_cluster = EXFAT_FIRST_CLUSTER + l->bitmap_clusters + l->upcase_clusters;
	return EXFAT_OK;
}

static ExfatStatus write_device(const Layout *l, uint64_t sector, uint32_t count, const void *buffer)
{
	return l->device->write(l->device->context, sector, count, buffer) ? EXFAT_IO : EXFAT_OK;
}

// Zeros `count` device sectors; the scratch buffer is zero afterwards.
static ExfatStatus write_zeros(const Layout *l, uint64_t sector, uint64_t count)
{
	const uint32_t per_write = SCRATCH_BYTES >> l->sector_shift;
	memset(scratch, 0, sizeof(scratch));
	while (count) {
		const uint32_t n = count < per_write ? (uint32_t)count : per_write;
		const ExfatStatus status = write_device(l, sector, n, scratch);
		if (status != EXFAT_OK)
			return status;
		sector += n;
		count -= n;
	}
	return EXFAT_OK;
}

// Writes the scratch buffer's head at a volume sector, then zeros up to `sectors` in all.
static ExfatStatus write_region(const Layout *l, uint64_t volume_sector, uint64_t sectors)
{
	const uint32_t per_write = SCRATCH_BYTES >> l->sector_shift;
	const uint32_t head = sectors < per_write ? (uint32_t)sectors : per_write;
	const uint64_t sector = l->partition_sector + volume_sector;
	const ExfatStatus status = write_device(l, sector, head, scratch);
	if (status != EXFAT_OK)
		return status;
	return write_zeros(l, sector + head, sectors - head);
}

static void build_boot_sector(const Layout *l, uint8_t *boot)
{
	boot[0] = 0xEB;
	boot[1] = 0x76;
	boot[2] = 0x90;
	memcpy(boot + EXFAT_BOOT_NAME, "EXFAT   ", 8);
	exfat_put64(boot + EXFAT_BOOT_PARTITION_OFFSET, l->partition_sector);
	exfat_put64(boot + EXFAT_BOOT_VOLUME_LENGTH, l->volume_sectors);
	exfat_put32(boot + EXFAT_BOOT_FAT_OFFSET, l->fat_sector);
	exfat_put32(boot + EXFAT_BOOT_FAT_LENGTH, l->fat_sectors);
	exfat_put32(boot + EXFAT_BOOT_HEAP_OFFSET, l->heap_sector);
	exfat_put32(boot + EXFAT_BOOT_CLUSTER_COUNT, l->cluster_count);
	exfat_put32(boot + EXFAT_BOOT_ROOT_CLUSTER, l->root_cluster);
	exfat_put32(boot + EXFAT_BOOT_SERIAL, l->serial);
	exfat_put16(boot + EXFAT_BOOT_REVISION, 0x0100);
	boot[EXFAT_BOOT_SECTOR_SHIFT] = (uint8_t)l->sector_shift;
	boot[EXFAT_BOOT_CLUSTER_SHIFT] = (uint8_t)l->cluster_shift;
	boot[EXFAT_BOOT_FAT_COUNT] = 1;
	boot[EXFAT_BOOT_DRIVE_SELECT] = 0x80;
	boot[EXFAT_BOOT_SIGNATURE] = 0x55;
	boot[EXFAT_BOOT_SIGNATURE + 1] = 0xAA;
}

// The main boot region and its backup: the boot sector, eight extended boot sectors carrying only their signature, the
// zero OEM and reserved sectors, and the checksum sector.
static ExfatStatus write_boot_regions(const Layout *l)
{
	const uint32_t sector_bytes = 1u << l->sector_shift;
	memset(scratch, 0, sizeof(scratch));
	build_boot_sector(l, scratch);
	for (uint32_t i = 1; i <= 8; i++) {
		uint8_t *const tail = scratch + (i + 1) * sector_bytes - 4;
		tail[2] = 0x55;
		tail[3] = 0xAA;
	}
	const uint32_t checksum = exfat_checksum32(0, scratch, (BOOT_REGION_SECTORS - 1) * sector_bytes, true);
	uint8_t *const checksum_sector = scratch + (BOOT_REGION_SECTORS - 1) * sector_bytes;
	for (uint32_t i = 0; i < sector_bytes; i += 4)
		exfat_put32(checksum_sector + i, checksum);
	ExfatStatus status = write_device(l, l->partition_sector, BOOT_REGION_SECTORS, scratch);
	if (status == EXFAT_OK)
		status = write_device(l, l->partition_sector + BOOT_REGION_SECTORS, BOOT_REGION_SECTORS, scratch);
	return status;
}

// The FAT: the media mark, then chains for the bitmap, the up-case table and the root directory (spec section 4.1).
static ExfatStatus write_fat(const Layout *l)
{
	memset(scratch, 0, sizeof(scratch));
	exfat_put32(scratch, 0xFFFFFFF8);
	exfat_put32(scratch + 4, EXFAT_CLUSTER_END);
	for (uint32_t cluster = EXFAT_FIRST_CLUSTER; cluster <= l->root_cluster; cluster++) {
		const uint32_t upcase_first = EXFAT_FIRST_CLUSTER + l->bitmap_clusters;
		const bool last = cluster + 1 == upcase_first || cluster + 1 == l->root_cluster || cluster == l->root_cluster;
		exfat_put32(scratch + 4 * cluster, last ? EXFAT_CLUSTER_END : cluster + 1);
	}
	return write_region(l, l->fat_sector, l->fat_sectors);
}

// The bitmap, with clusters 2 to the root allocated, and the up-case table in the clusters after it.
static ExfatStatus write_bitmap_and_upcase(const Layout *l)
{
	memset(scratch, 0, sizeof(scratch));
	for (uint32_t cluster = EXFAT_FIRST_CLUSTER; cluster <= l->root_cluster; cluster++)
		scratch[(cluster - EXFAT_FIRST_CLUSTER) / 8] |= (uint8_t)(1 << (cluster - EXFAT_FIRST_CLUSTER) % 8);
	ExfatStatus status = write_region(l, l->heap_sector, (uint64_t)l->bitmap_clusters << l->cluster_shift);
	if (status != EXFAT_OK)
		return status;
	memset(scratch, 0, sizeof(scratch));
	for (uint32_t i = 0; i < EXFAT_UPCASE_TABLE_WORDS; i++)
		exfat_put16(scratch + 2 * i, exfat_upcase_table[i]);
	const uint64_t upcase_sector = l->heap_sector + ((uint64_t)l->bitmap_clusters << l->cluster_shift);
	return write_region(l, upcase_sector, (uint64_t)l->upcase_clusters << l->cluster_shift);
}

// The root directory: the label when there is one, the bitmap entry and the up-case entry.
static ExfatStatus write_root(const Layout *l, const char *label, uint32_t upcase_checksum)
{
	memset(scratch, 0, sizeof(scratch));
	uint8_t *entry = scratch;
	const size_t label_length = strlen(label);
	if (label_length) {
		entry[0] = EXFAT_TYPE_LABEL;
		entry[EXFAT_LABEL_COUNT] = (uint8_t)label_length;
		for (size_t i = 0; i < label_length; i++)
			exfat_put16(entry + EXFAT_LABEL_TEXT + 2 * i, (uint8_t)label[i]);
		entry += EXFAT_ENTRY_BYTES;
	}
	entry[0] = EXFAT_TYPE_BITMAP;
	exfat_put32(entry + EXFAT_ALLOCATION_FIRST_CLUSTER, EXFAT_FIRST_CLUSTER);
	exfat_put64(entry + EXFAT_ALLOCATION_DATA_LENGTH, ((uint64_t)l->cluster_count + 7) / 8);
	entry += EXFAT_ENTRY_BYTES;
	entry[0] = EXFAT_TYPE_UPCASE;
	exfat_put32(entry + EXFAT_UPCASE_CHECKSUM, upcase_checksum);
	exfat_put32(entry + EXFAT_ALLOCATION_FIRST_CLUSTER, EXFAT_FIRST_CLUSTER + l->bitmap_clusters);
	exfat_put64(entry + EXFAT_ALLOCATION_DATA_LENGTH, UPCASE_BYTES);
	const uint64_t root_sector =
		l->heap_sector + ((uint64_t)(l->root_cluster - EXFAT_FIRST_CLUSTER) << l->cluster_shift);
	return write_region(l, root_sector, (uint64_t)1 << l->cluster_shift);
}

// An MBR's CHS bytes for a sector: 255 heads of 63 sectors, saturated as Windows writes it past cylinder 1023.
static void put_chs(uint8_t *bytes, uint64_t sector)
{
	const uint64_t cylinder = sector / (255 * 63);
	if (cylinder > 1023) {
		bytes[0] = 0xFE;
		bytes[1] = 0xFF;
		bytes[2] = 0xFF;
		return;
	}
	bytes[0] = (uint8_t)(sector / 63 % 255);
	bytes[1] = (uint8_t)((sector % 63 + 1) | (cylinder >> 8 & 3) << 6);
	bytes[2] = (uint8_t)cylinder;
}

static ExfatStatus write_mbr(const Layout *l)
{
	memset(scratch, 0, sizeof(scratch));
	exfat_put32(scratch + MBR_DISK_SIGNATURE, l->serial);
	uint8_t *const entry = scratch + MBR_PARTITION_TABLE;
	put_chs(entry + 1, l->partition_sector);
	entry[4] = MBR_TYPE_EXFAT;
	put_chs(entry + 5, l->partition_sector + l->volume_sectors - 1);
	exfat_put32(entry + 8, (uint32_t)l->partition_sector);
	exfat_put32(entry + 12, (uint32_t)l->volume_sectors);
	scratch[EXFAT_BOOT_SIGNATURE] = 0x55;
	scratch[EXFAT_BOOT_SIGNATURE + 1] = 0xAA;
	return write_device(l, 0, 1, scratch);
}

ExfatStatus xsdk_exfat_format(const ExfatDevice *device, const char *label, uint32_t serial)
{
	if (strlen(label) > EXFAT_LABEL_MAX)
		return EXFAT_NAME;
	Layout l;
	ExfatStatus status = plan(device, serial, &l);
	if (status != EXFAT_OK)
		return status;
	if (l.partition_sector + l.volume_sectors > UINT32_MAX)
		return EXFAT_UNSUPPORTED;
	uint8_t upcase[UPCASE_BYTES];
	for (uint32_t i = 0; i < EXFAT_UPCASE_TABLE_WORDS; i++)
		exfat_put16(upcase + 2 * i, exfat_upcase_table[i]);
	const uint32_t upcase_checksum = exfat_checksum32(0, upcase, UPCASE_BYTES, false);
	// The MBR goes last: until it is there, nothing points at a half-written volume.
	status = write_zeros(&l, 1, l.partition_sector - 1);
	if (status == EXFAT_OK)
		status = write_fat(&l);
	if (status == EXFAT_OK)
		status = write_bitmap_and_upcase(&l);
	if (status == EXFAT_OK)
		status = write_root(&l, label, upcase_checksum);
	if (status == EXFAT_OK)
		status = write_boot_regions(&l);
	if (status == EXFAT_OK)
		status = write_mbr(&l);
	return status;
}
