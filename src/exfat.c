#include "xsdk/exfat.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "exfat_private.h"

// The volume over the device's two callbacks (docs/scope/77-exfat.md, step 2). Metadata goes through a cache of 4 KB
// blocks written back in order, the FAT first, the bitmap next, directories last, so a power cut leaks clusters rather
// than leaves an entry pointing at unallocated ones; an entry is written before its clusters are freed for the same
// reason. File data goes straight between the caller's buffer and the device, through the bounce sector where it
// starts or ends inside one.

enum {
	SET_ENTRIES_MAX = 2 + (EXFAT_NAME_MAX + EXFAT_NAME_ENTRY_CHARS - 1) / EXFAT_NAME_ENTRY_CHARS,
	SET_BYTES_MAX = SET_ENTRIES_MAX * EXFAT_ENTRY_BYTES,
	// An operation that leaves this many blocks dirty ends with a flush.
	DIRTY_BLOCKS_FLUSH = 32,
	MBR_PARTITION_TABLE = 446,
	MBR_PARTITION_ENTRY_BYTES = 16,
	MBR_TYPE_EXFAT = 0x07,
	// A timestamp's UtcOffset byte saying "valid, UTC".
	UTC_OFFSET_ZERO = 0x80,
};

// A directory reached through a child's set, whose size the child doesn't know.
static const uint64_t SIZE_UNKNOWN = UINT64_MAX;
// 1980-01-01, exFAT's first second, as a FILETIME: 138426 days of 86400 s in 100 ns units.
static const uint64_t FILETIME_1980 = 119600064000000000ULL;
static const uint64_t FILETIME_SECOND = 10000000;
static const int64_t DAYS_1601_TO_1970 = 134774;
static const char *const status_names[] = { "ok", "device error", "not exFAT", "corrupt", "unsupported", "not found",
	"exists", "not a directory", "is a directory", "not empty", "full", "bad name" };

static uint32_t sector_bytes_of(const ExfatVolume *v)
{
	return 1u << v->sector_shift;
}

static uint64_t cluster_bytes_of(const ExfatVolume *v)
{
	return (uint64_t)1 << v->cluster_shift;
}

static uint32_t clusters_for(const ExfatVolume *v, uint64_t bytes)
{
	return (uint32_t)((bytes + cluster_bytes_of(v) - 1) >> v->cluster_shift);
}

static bool cluster_valid(const ExfatVolume *v, uint32_t cluster)
{
	return cluster >= EXFAT_FIRST_CLUSTER && cluster - EXFAT_FIRST_CLUSTER < v->cluster_count;
}

static uint64_t cluster_offset(const ExfatVolume *v, uint32_t cluster)
{
	return v->heap_offset + ((uint64_t)(cluster - EXFAT_FIRST_CLUSTER) << v->cluster_shift);
}

static uint64_t now(const ExfatVolume *v)
{
	return v->device.now ? v->device.now(v->device.context) : FILETIME_1980;
}

// Whole sectors at a byte offset in the volume.
static ExfatStatus read_volume(ExfatVolume *v, uint64_t offset, uint32_t bytes, void *buffer)
{
	if (offset + bytes > v->volume_bytes)
		return EXFAT_CORRUPT;
	const uint64_t sector = v->device.first_sector + (offset >> v->sector_shift);
	return v->device.read(v->device.context, sector, bytes >> v->sector_shift, buffer) ? EXFAT_IO : EXFAT_OK;
}

static ExfatStatus write_volume(ExfatVolume *v, uint64_t offset, uint32_t bytes, const void *buffer)
{
	if (offset + bytes > v->volume_bytes)
		return EXFAT_CORRUPT;
	const uint64_t sector = v->device.first_sector + (offset >> v->sector_shift);
	return v->device.write(v->device.context, sector, bytes >> v->sector_shift, buffer) ? EXFAT_IO : EXFAT_OK;
}

// The cache.

// A block's bytes within the volume: the last one may be short.
static uint32_t block_bytes_at(const ExfatVolume *v, uint64_t block)
{
	const uint64_t remaining = v->volume_bytes - block * EXFAT_CACHE_BLOCK_BYTES;
	return remaining < EXFAT_CACHE_BLOCK_BYTES ? (uint32_t)remaining : EXFAT_CACHE_BLOCK_BYTES;
}

// 0 for the FAT, 1 for the bitmap, 2 for the rest: the order the cache writes back in.
static unsigned block_region(const ExfatVolume *v, uint64_t block)
{
	const uint64_t offset = block * EXFAT_CACHE_BLOCK_BYTES;
	if (offset >= v->fat_offset && offset < v->fat_offset + v->fat_bytes)
		return 0;
	if (v->bitmap_cluster) {
		const uint64_t bitmap = cluster_offset(v, v->bitmap_cluster);
		if (offset >= bitmap && offset < bitmap + v->bitmap_bytes)
			return 1;
	}
	return 2;
}

static ExfatStatus flush_slot(ExfatVolume *v, uint32_t index)
{
	ExfatCacheSlot *const slot = &v->slots[index];
	const ExfatStatus status =
		write_volume(v, slot->block * EXFAT_CACHE_BLOCK_BYTES, block_bytes_at(v, slot->block), v->blocks[index]);
	if (status != EXFAT_OK)
		return status;
	slot->dirty = false;
	v->dirty_blocks--;
	return EXFAT_OK;
}

static ExfatStatus cache_flush(ExfatVolume *v)
{
	for (unsigned region = 0; region < 3 && v->dirty_blocks; region++) {
		for (uint32_t i = 0; i < EXFAT_CACHE_BLOCKS; i++) {
			if (!v->slots[i].dirty || block_region(v, v->slots[i].block) != region)
				continue;
			const ExfatStatus status = flush_slot(v, i);
			if (status != EXFAT_OK)
				return status;
		}
	}
	return EXFAT_OK;
}

// The bytes at a volume offset, in the block that holds them, loaded if needed. The pointer lasts until the next cache
// call, which may evict the block.
static ExfatStatus cached(ExfatVolume *v, uint64_t offset, uint8_t **bytes)
{
	const uint64_t key = offset / EXFAT_CACHE_BLOCK_BYTES;
	uint32_t victim = EXFAT_CACHE_BLOCKS;
	for (uint32_t i = 0; i < EXFAT_CACHE_BLOCKS; i++) {
		ExfatCacheSlot *const slot = &v->slots[i];
		if (slot->block == key) {
			slot->used = ++v->tick;
			*bytes = v->blocks[i] + offset % EXFAT_CACHE_BLOCK_BYTES;
			return EXFAT_OK;
		}
		if (!slot->dirty && (victim == EXFAT_CACHE_BLOCKS || slot->used < v->slots[victim].used))
			victim = i;
	}
	if (victim == EXFAT_CACHE_BLOCKS) {
		const ExfatStatus status = cache_flush(v);
		if (status != EXFAT_OK)
			return status;
		victim = 0;
		for (uint32_t i = 1; i < EXFAT_CACHE_BLOCKS; i++) {
			if (v->slots[i].used < v->slots[victim].used)
				victim = i;
		}
	}
	ExfatCacheSlot *const slot = &v->slots[victim];
	const uint32_t bytes_in_volume = block_bytes_at(v, key);
	slot->block = UINT64_MAX;
	const ExfatStatus status = read_volume(v, key * EXFAT_CACHE_BLOCK_BYTES, bytes_in_volume, v->blocks[victim]);
	if (status != EXFAT_OK)
		return status;
	memset(v->blocks[victim] + bytes_in_volume, 0, EXFAT_CACHE_BLOCK_BYTES - bytes_in_volume);
	slot->block = key;
	slot->used = ++v->tick;
	slot->dirty = false;
	*bytes = v->blocks[victim] + offset % EXFAT_CACHE_BLOCK_BYTES;
	return EXFAT_OK;
}

static void cache_dirty(ExfatVolume *v, const uint8_t *bytes)
{
	ExfatCacheSlot *const slot = &v->slots[(bytes - v->blocks[0]) / EXFAT_CACHE_BLOCK_BYTES];
	if (slot->dirty)
		return;
	slot->dirty = true;
	v->dirty_blocks++;
}

// Forgets the blocks of clusters being freed, so nothing stale is written over their next use.
static void cache_drop(ExfatVolume *v, uint64_t offset, uint64_t bytes)
{
	const uint64_t first = offset / EXFAT_CACHE_BLOCK_BYTES;
	const uint64_t last = (offset + bytes - 1) / EXFAT_CACHE_BLOCK_BYTES;
	for (uint32_t i = 0; i < EXFAT_CACHE_BLOCKS; i++) {
		ExfatCacheSlot *const slot = &v->slots[i];
		if (slot->block < first || slot->block > last)
			continue;
		if (slot->dirty)
			v->dirty_blocks--;
		slot->block = UINT64_MAX;
		slot->dirty = false;
	}
}

// Ends an operation: a flush once enough is dirty.
static ExfatStatus settle(ExfatVolume *v)
{
	return v->dirty_blocks >= DIRTY_BLOCKS_FLUSH ? cache_flush(v) : EXFAT_OK;
}

// Sets or clears VolumeDirty in the boot sector, which the boot checksum leaves out.
static ExfatStatus set_dirty_flag(ExfatVolume *v, bool dirty)
{
	if (v->dirty == dirty)
		return EXFAT_OK;
	ExfatStatus status = read_volume(v, 0, sector_bytes_of(v), v->bounce);
	if (status != EXFAT_OK)
		return status;
	const uint16_t flags = exfat_le16(v->bounce + EXFAT_BOOT_FLAGS);
	exfat_put16(v->bounce + EXFAT_BOOT_FLAGS, dirty ? flags | EXFAT_FLAG_DIRTY : flags & ~EXFAT_FLAG_DIRTY);
	status = write_volume(v, 0, sector_bytes_of(v), v->bounce);
	if (status != EXFAT_OK)
		return status;
	v->dirty = dirty;
	v->dirtied = dirty;
	return EXFAT_OK;
}

// The FAT and the bitmap.

static ExfatStatus fat_read(ExfatVolume *v, uint32_t cluster, uint32_t *next)
{
	uint8_t *bytes;
	const ExfatStatus status = cached(v, v->fat_offset + (uint64_t)cluster * 4, &bytes);
	if (status != EXFAT_OK)
		return status;
	*next = exfat_le32(bytes);
	return EXFAT_OK;
}

static ExfatStatus fat_write(ExfatVolume *v, uint32_t cluster, uint32_t value)
{
	uint8_t *bytes;
	const ExfatStatus status = cached(v, v->fat_offset + (uint64_t)cluster * 4, &bytes);
	if (status != EXFAT_OK)
		return status;
	exfat_put32(bytes, value);
	cache_dirty(v, bytes);
	return EXFAT_OK;
}

static ExfatStatus bitmap_byte(ExfatVolume *v, uint32_t cluster, uint8_t **byte)
{
	return cached(v, cluster_offset(v, v->bitmap_cluster) + (cluster - EXFAT_FIRST_CLUSTER) / 8, byte);
}

static ExfatStatus bitmap_set(ExfatVolume *v, uint32_t cluster, bool allocated)
{
	if (!cluster_valid(v, cluster))
		return EXFAT_CORRUPT;
	uint8_t *byte;
	const ExfatStatus status = bitmap_byte(v, cluster, &byte);
	if (status != EXFAT_OK)
		return status;
	const uint8_t mask = (uint8_t)(1 << (cluster - EXFAT_FIRST_CLUSTER) % 8);
	if (((*byte & mask) != 0) == allocated)
		return EXFAT_CORRUPT;
	*byte = allocated ? *byte | mask : *byte & ~mask;
	cache_dirty(v, byte);
	v->free_clusters += allocated ? -1 : 1;
	if (allocated)
		v->next_free = cluster + 1;
	return EXFAT_OK;
}

// The first free cluster at or after `from`; 0 when there is none before the end.
static ExfatStatus bitmap_next_free(ExfatVolume *v, uint32_t from, uint32_t *cluster)
{
	const uint32_t end = EXFAT_FIRST_CLUSTER + v->cluster_count;
	for (uint32_t c = from; c < end;) {
		uint8_t *byte;
		const ExfatStatus status = bitmap_byte(v, c, &byte);
		if (status != EXFAT_OK)
			return status;
		const uint32_t bit = (c - EXFAT_FIRST_CLUSTER) % 8;
		if (*byte == 0xFF) {
			c += 8 - bit;
			continue;
		}
		if (!(*byte >> bit & 1)) {
			*cluster = c;
			return EXFAT_OK;
		}
		c++;
	}
	*cluster = 0;
	return EXFAT_OK;
}

// How many clusters from `cluster` on are free, up to `limit`.
static ExfatStatus bitmap_run(ExfatVolume *v, uint32_t cluster, uint32_t limit, uint32_t *run)
{
	const uint32_t end = EXFAT_FIRST_CLUSTER + v->cluster_count;
	*run = 0;
	for (uint32_t c = cluster; c < end && *run < limit;) {
		uint8_t *byte;
		const ExfatStatus status = bitmap_byte(v, c, &byte);
		if (status != EXFAT_OK)
			return status;
		const uint32_t bit = (c - EXFAT_FIRST_CLUSTER) % 8;
		if (*byte == 0 && bit == 0 && c + 8 <= end && *run + 8 <= limit) {
			*run += 8;
			c += 8;
			continue;
		}
		if (*byte >> bit & 1)
			return EXFAT_OK;
		(*run)++;
		c++;
	}
	return EXFAT_OK;
}

// A run of `count` free clusters, searched from the hint and wrapping once.
static ExfatStatus find_free_run(ExfatVolume *v, uint32_t count, uint32_t *first)
{
	const uint32_t volume_end = EXFAT_FIRST_CLUSTER + v->cluster_count;
	const uint32_t start = cluster_valid(v, v->next_free) ? v->next_free : EXFAT_FIRST_CLUSTER;
	for (unsigned pass = 0; pass < 2; pass++) {
		uint32_t c = pass ? EXFAT_FIRST_CLUSTER : start;
		const uint32_t end = pass ? start : volume_end;
		while (c < end) {
			ExfatStatus status = bitmap_next_free(v, c, &c);
			if (status != EXFAT_OK)
				return status;
			if (!c || c >= end)
				break;
			uint32_t run;
			status = bitmap_run(v, c, count, &run);
			if (status != EXFAT_OK)
				return status;
			if (run >= count) {
				*first = c;
				return EXFAT_OK;
			}
			c += run;
		}
	}
	return EXFAT_FULL;
}

// Clusters and data.

static uint32_t node_clusters(const ExfatVolume *v, const ExfatNode *node)
{
	if (!node->first_cluster)
		return 0;
	return node->size_bytes == SIZE_UNKNOWN ? UINT32_MAX : clusters_for(v, node->size_bytes);
}

// The cluster holding a node's data at cluster index `index`, and how many clusters from it on lie in a row on disk.
static ExfatStatus node_map(ExfatVolume *v, ExfatNode *node, uint32_t index, uint32_t *cluster, uint32_t *run)
{
	const uint32_t clusters = node_clusters(v, node);
	if (index >= clusters)
		return EXFAT_CORRUPT;
	if (node->contiguous) {
		*cluster = node->first_cluster + index;
		*run = clusters - index;
		if (!cluster_valid(v, *cluster))
			return EXFAT_CORRUPT;
		if (clusters != UINT32_MAX &&
			(uint64_t)node->first_cluster + clusters - 1 >= EXFAT_FIRST_CLUSTER + v->cluster_count)
			return EXFAT_CORRUPT;
		return EXFAT_OK;
	}
	uint32_t i = 0;
	uint32_t c = node->first_cluster;
	if (node->walk_cluster && node->walk_index <= index) {
		i = node->walk_index;
		c = node->walk_cluster;
	}
	for (; i < index; i++) {
		if (!cluster_valid(v, c))
			return EXFAT_CORRUPT;
		const ExfatStatus status = fat_read(v, c, &c);
		if (status != EXFAT_OK)
			return status;
	}
	if (!cluster_valid(v, c))
		return EXFAT_CORRUPT;
	node->walk_index = index;
	node->walk_cluster = c;
	*cluster = c;
	*run = 1;
	while (index + *run < clusters) {
		uint32_t next;
		const ExfatStatus status = fat_read(v, c, &next);
		if (status != EXFAT_OK)
			return status;
		if (next != c + 1)
			break;
		c = next;
		(*run)++;
	}
	return EXFAT_OK;
}

// Moves bytes between a node's data and the buffer (NULL writes zeros): whole sectors straight, the rest through the
// bounce sector.
static ExfatStatus transfer(
	ExfatVolume *v, ExfatNode *node, uint64_t offset, uint8_t *buffer, uint64_t bytes, bool write)
{
	const uint32_t sector_bytes = sector_bytes_of(v);
	while (bytes) {
		uint32_t cluster;
		uint32_t run;
		ExfatStatus status = node_map(v, node, (uint32_t)(offset >> v->cluster_shift), &cluster, &run);
		if (status != EXFAT_OK)
			return status;
		const uint64_t within = offset & (cluster_bytes_of(v) - 1);
		const uint64_t device_offset = cluster_offset(v, cluster) + within;
		uint64_t chunk = ((uint64_t)run << v->cluster_shift) - within;
		if (chunk > bytes)
			chunk = bytes;
		const uint32_t in_sector = (uint32_t)(device_offset & (sector_bytes - 1));
		uint64_t moved;
		if (in_sector || chunk < sector_bytes) {
			moved = sector_bytes - in_sector;
			if (moved > chunk)
				moved = chunk;
			status = read_volume(v, device_offset - in_sector, sector_bytes, v->bounce);
			if (status != EXFAT_OK)
				return status;
			if (!write) {
				memcpy(buffer, v->bounce + in_sector, (size_t)moved);
			} else {
				if (buffer)
					memcpy(v->bounce + in_sector, buffer, (size_t)moved);
				else
					memset(v->bounce + in_sector, 0, (size_t)moved);
				status = write_volume(v, device_offset - in_sector, sector_bytes, v->bounce);
			}
		} else if (!buffer) {
			moved = chunk & ~(uint64_t)(sector_bytes - 1);
			if (moved > EXFAT_SECTOR_BYTES_MAX)
				moved = EXFAT_SECTOR_BYTES_MAX;
			memset(v->bounce, 0, (size_t)moved);
			status = write_volume(v, device_offset, (uint32_t)moved, v->bounce);
		} else {
			moved = chunk & ~(uint64_t)(sector_bytes - 1);
			if (moved > UINT32_MAX - EXFAT_SECTOR_BYTES_MAX)
				moved = UINT32_MAX - EXFAT_SECTOR_BYTES_MAX + 1;
			status = write ? write_volume(v, device_offset, (uint32_t)moved, buffer)
						   : read_volume(v, device_offset, (uint32_t)moved, buffer);
		}
		if (status != EXFAT_OK)
			return status;
		offset += moved;
		bytes -= moved;
		if (buffer)
			buffer += moved;
	}
	return EXFAT_OK;
}

static ExfatStatus zero_cluster(ExfatVolume *v, uint32_t cluster)
{
	const uint64_t offset = cluster_offset(v, cluster);
	cache_drop(v, offset, cluster_bytes_of(v));
	memset(v->bounce, 0, sizeof(v->bounce));
	for (uint64_t done = 0; done < cluster_bytes_of(v); done += sizeof(v->bounce)) {
		uint64_t piece = cluster_bytes_of(v) - done;
		if (piece > sizeof(v->bounce))
			piece = sizeof(v->bounce);
		const ExfatStatus status = write_volume(v, offset + done, (uint32_t)piece, v->bounce);
		if (status != EXFAT_OK)
			return status;
	}
	return EXFAT_OK;
}

static ExfatStatus take_cluster(ExfatVolume *v, uint32_t cluster)
{
	return bitmap_set(v, cluster, true);
}

// Allocates clusters until the node has `needed`. A contiguous node stays so while the clusters after it are free;
// otherwise its clusters get a FAT chain and the new ones join it one by one.
static ExfatStatus extend(ExfatVolume *v, ExfatNode *node, uint32_t needed)
{
	const uint32_t have = node_clusters(v, node);
	if (needed <= have)
		return EXFAT_OK;
	const uint32_t extra = needed - have;
	if (extra > v->free_clusters)
		return EXFAT_FULL;
	ExfatStatus status;
	uint32_t last;
	if (!have) {
		uint32_t first;
		if (find_free_run(v, extra, &first) == EXFAT_OK) {
			for (uint32_t i = 0; i < extra; i++) {
				status = take_cluster(v, first + i);
				if (status != EXFAT_OK)
					return status;
			}
			node->first_cluster = first;
			node->contiguous = true;
			return EXFAT_OK;
		}
		status = find_free_run(v, 1, &first);
		if (status != EXFAT_OK)
			return status;
		status = take_cluster(v, first);
		if (status != EXFAT_OK)
			return status;
		node->first_cluster = first;
		node->contiguous = false;
		last = first;
		for (uint32_t i = 1; i < extra; i++) {
			uint32_t next;
			status = find_free_run(v, 1, &next);
			if (status == EXFAT_OK)
				status = take_cluster(v, next);
			if (status == EXFAT_OK)
				status = fat_write(v, last, next);
			if (status != EXFAT_OK)
				return status;
			last = next;
		}
		return fat_write(v, last, EXFAT_CLUSTER_END);
	}
	if (node->contiguous) {
		uint32_t run;
		status = bitmap_run(v, node->first_cluster + have, extra, &run);
		if (status != EXFAT_OK)
			return status;
		if (run >= extra) {
			for (uint32_t i = 0; i < extra; i++) {
				status = take_cluster(v, node->first_cluster + have + i);
				if (status != EXFAT_OK)
					return status;
			}
			return EXFAT_OK;
		}
		for (uint32_t i = 0; i + 1 < have; i++) {
			status = fat_write(v, node->first_cluster + i, node->first_cluster + i + 1);
			if (status != EXFAT_OK)
				return status;
		}
		node->contiguous = false;
		last = node->first_cluster + have - 1;
	} else {
		uint32_t run;
		status = node_map(v, node, have - 1, &last, &run);
		if (status != EXFAT_OK)
			return status;
	}
	for (uint32_t i = 0; i < extra; i++) {
		uint32_t next;
		status = find_free_run(v, 1, &next);
		if (status == EXFAT_OK)
			status = take_cluster(v, next);
		if (status == EXFAT_OK)
			status = fat_write(v, last, next);
		if (status != EXFAT_OK)
			return status;
		last = next;
	}
	return fat_write(v, last, EXFAT_CLUSTER_END);
}

// Frees the node's clusters from index `keep` on; `have` is how many it has.
static ExfatStatus release(ExfatVolume *v, ExfatNode *node, uint32_t have, uint32_t keep)
{
	if (keep >= have)
		return EXFAT_OK;
	ExfatStatus status = EXFAT_OK;
	if (node->contiguous) {
		for (uint32_t i = keep; i < have && status == EXFAT_OK; i++)
			status = bitmap_set(v, node->first_cluster + i, false);
	} else {
		uint32_t c = node->first_cluster;
		if (keep) {
			uint32_t run;
			status = node_map(v, node, keep - 1, &c, &run);
			uint32_t next = 0;
			if (status == EXFAT_OK)
				status = fat_read(v, c, &next);
			if (status == EXFAT_OK)
				status = fat_write(v, c, EXFAT_CLUSTER_END);
			c = next;
		}
		for (uint32_t i = keep; i < have && status == EXFAT_OK; i++) {
			if (!cluster_valid(v, c))
				return EXFAT_CORRUPT;
			uint32_t next;
			status = fat_read(v, c, &next);
			if (status == EXFAT_OK)
				status = bitmap_set(v, c, false);
			c = next;
		}
	}
	if (!keep) {
		node->first_cluster = 0;
		node->contiguous = false;
	}
	node->walk_index = 0;
	node->walk_cluster = 0;
	return status;
}

// Names and times.

static uint16_t upcase_char(const ExfatVolume *v, uint16_t c)
{
	uint32_t position = 0;
	for (uint32_t i = 0; i < v->upcase_words; i++) {
		const uint16_t word = v->upcase[i];
		if (word == 0xFFFF && i + 1 < v->upcase_words) {
			const uint32_t skip = v->upcase[++i];
			if (c < position + skip)
				return c;
			position += skip;
			continue;
		}
		if (position == c)
			return word;
		position++;
	}
	return c;
}

static uint16_t name_hash(const ExfatVolume *v, const uint16_t *name, uint32_t length)
{
	uint16_t hash = 0;
	for (uint32_t i = 0; i < length; i++) {
		const uint16_t c = upcase_char(v, name[i]);
		const uint8_t bytes[2] = { (uint8_t)c, (uint8_t)(c >> 8) };
		hash = exfat_checksum16(hash, bytes, 2, false);
	}
	return hash;
}

static bool names_equal(const ExfatVolume *v, const uint16_t *a, const uint16_t *b, uint32_t length)
{
	for (uint32_t i = 0; i < length; i++) {
		if (upcase_char(v, a[i]) != upcase_char(v, b[i]))
			return false;
	}
	return true;
}

static bool is_separator(char c)
{
	return c == '\\' || c == '/';
}

// A path component as UTF-16; false when exFAT or Latin-1 can't take it.
static bool component_name(const char *text, uint32_t length, uint16_t *name)
{
	if (!length || length > EXFAT_NAME_MAX)
		return false;
	if (text[0] == '.' && (length == 1 || (length == 2 && text[1] == '.')))
		return false;
	for (uint32_t i = 0; i < length; i++) {
		const uint8_t c = (uint8_t)text[i];
		if (c < 0x20 || c == '"' || c == '*' || c == ':' || c == '<' || c == '>' || c == '?' || c == '|' ||
			is_separator((char)c))
			return false;
		name[i] = c;
	}
	return true;
}

// The parent's length and the last component of a path; false without a component.
static bool split_path(const char *path, uint32_t *parent_length, const char **leaf, uint32_t *leaf_length)
{
	uint32_t end = (uint32_t)strlen(path);
	while (end && is_separator(path[end - 1]))
		end--;
	if (!end)
		return false;
	uint32_t start = end;
	while (start && !is_separator(path[start - 1]))
		start--;
	*parent_length = start;
	*leaf = path + start;
	*leaf_length = end - start;
	return true;
}

// Days since 1970-01-01 to a civil date and back (Howard Hinnant's algorithms).
static void civil_from_days(int64_t z, uint32_t *year, uint32_t *month, uint32_t *day)
{
	z += 719468;
	const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
	const uint32_t doe = (uint32_t)(z - era * 146097);
	const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	const uint32_t mp = (5 * doy + 2) / 153;
	*day = doy - (153 * mp + 2) / 5 + 1;
	*month = mp < 10 ? mp + 3 : mp - 9;
	*year = (uint32_t)(yoe + era * 400 + (*month <= 2));
}

static int64_t days_from_civil(int64_t year, uint32_t month, uint32_t day)
{
	year -= month <= 2;
	const int64_t era = (year >= 0 ? year : year - 399) / 400;
	const uint32_t yoe = (uint32_t)(year - era * 400);
	const uint32_t doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
	const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (int64_t)doe - 719468;
}

// A FILETIME as exFAT's timestamp (7.4.8) and 10 ms increment, clamped to 1980 to 2107.
static void filetime_to_stamp(uint64_t filetime, uint32_t *stamp, uint8_t *increment)
{
	if (filetime < FILETIME_1980)
		filetime = FILETIME_1980;
	const uint64_t seconds = filetime / FILETIME_SECOND;
	const uint32_t hundredths = (uint32_t)(filetime % FILETIME_SECOND / (FILETIME_SECOND / 100));
	uint32_t year;
	uint32_t month;
	uint32_t day;
	civil_from_days((int64_t)(seconds / 86400) - DAYS_1601_TO_1970, &year, &month, &day);
	uint32_t second = (uint32_t)(seconds % 86400);
	if (year > 2107) {
		year = 2107;
		month = 12;
		day = 31;
		second = 86399;
	}
	*stamp =
		(year - 1980) << 25 | month << 21 | day << 16 | second / 3600 << 11 | second / 60 % 60 << 5 | second % 60 / 2;
	*increment = (uint8_t)(second % 2 * 100 + hundredths);
}

static uint64_t stamp_to_filetime(uint32_t stamp, uint8_t increment, uint8_t utc_offset)
{
	const uint32_t year = (stamp >> 25) + 1980;
	const uint32_t month = stamp >> 21 & 15;
	const uint32_t day = stamp >> 16 & 31;
	if (month < 1 || month > 12 || day < 1)
		return FILETIME_1980;
	int64_t seconds = (days_from_civil(year, month, day) + DAYS_1601_TO_1970) * 86400;
	seconds += (stamp >> 11 & 31) * 3600 + (stamp >> 5 & 63) * 60 + (stamp & 31) * 2 + increment / 100;
	int64_t filetime = seconds * (int64_t)FILETIME_SECOND + increment % 100 * (int64_t)(FILETIME_SECOND / 100);
	if (utc_offset & 0x80) {
		// A signed 7-bit count of 15-minute steps the local time is ahead of UTC.
		const int32_t quarters = (int8_t)(uint8_t)(utc_offset << 1) >> 1;
		filetime -= quarters * 15 * 60 * (int64_t)FILETIME_SECOND;
	}
	return filetime < 0 ? 0 : (uint64_t)filetime;
}

// Directories and entry sets.

static ExfatNode directory_of(uint32_t first_cluster, bool contiguous, uint64_t size_bytes)
{
	ExfatNode node;
	memset(&node, 0, sizeof(node));
	node.attributes = EXFAT_ATTRIBUTE_DIRECTORY;
	node.first_cluster = first_cluster;
	node.contiguous = contiguous;
	node.size_bytes = size_bytes;
	node.valid_bytes = size_bytes;
	return node;
}

static ExfatNode root_node(const ExfatVolume *v)
{
	return directory_of(v->root_cluster, false, v->root_bytes);
}

// The directory holding a node's set. Its flag may be stale once the directory grew into a chain, but the set lies in
// the clusters that were there before, which are still in a row.
static ExfatNode parent_of(const ExfatNode *node)
{
	return directory_of(node->parent_cluster, node->parent_contiguous, SIZE_UNKNOWN);
}

// A directory's entry at a byte offset, in the cache; EXFAT_NOT_FOUND past its end.
static ExfatStatus directory_entry(ExfatVolume *v, ExfatNode *dir, uint64_t offset, uint8_t **entry)
{
	if (offset + EXFAT_ENTRY_BYTES > dir->size_bytes)
		return EXFAT_NOT_FOUND;
	uint32_t cluster;
	uint32_t run;
	const ExfatStatus status = node_map(v, dir, (uint32_t)(offset >> v->cluster_shift), &cluster, &run);
	if (status != EXFAT_OK)
		return status;
	return cached(v, cluster_offset(v, cluster) + (offset & (cluster_bytes_of(v) - 1)), entry);
}

// The set whose file entry is at `offset`, copied out and checked; EXFAT_CORRUPT when it isn't one.
static ExfatStatus read_set(ExfatVolume *v, ExfatNode *dir, uint64_t offset, uint8_t *set, uint32_t *entries)
{
	uint8_t *entry;
	ExfatStatus status = directory_entry(v, dir, offset, &entry);
	if (status != EXFAT_OK)
		return status == EXFAT_NOT_FOUND ? EXFAT_CORRUPT : status;
	if (entry[0] != EXFAT_TYPE_FILE)
		return EXFAT_CORRUPT;
	const uint32_t count = 1u + entry[EXFAT_FILE_SECONDARY_COUNT];
	if (count < 3 || count > SET_ENTRIES_MAX)
		return EXFAT_CORRUPT;
	memcpy(set, entry, EXFAT_ENTRY_BYTES);
	for (uint32_t i = 1; i < count; i++) {
		status = directory_entry(v, dir, offset + i * EXFAT_ENTRY_BYTES, &entry);
		if (status != EXFAT_OK)
			return status == EXFAT_NOT_FOUND ? EXFAT_CORRUPT : status;
		memcpy(set + i * EXFAT_ENTRY_BYTES, entry, EXFAT_ENTRY_BYTES);
	}
	if (set[EXFAT_ENTRY_BYTES] != EXFAT_TYPE_STREAM)
		return EXFAT_CORRUPT;
	for (uint32_t i = 2; i < count; i++) {
		if ((set[i * EXFAT_ENTRY_BYTES] & 0xC0) != 0xC0)
			return EXFAT_CORRUPT;
	}
	if (exfat_checksum16(0, set, count * EXFAT_ENTRY_BYTES, true) != exfat_le16(set + EXFAT_FILE_CHECKSUM))
		return EXFAT_CORRUPT;
	*entries = count;
	return EXFAT_OK;
}

// A set's name in UTF-16; returns its length.
static uint32_t set_name(const uint8_t *set, uint32_t entries, uint16_t *name)
{
	const uint32_t length = set[EXFAT_ENTRY_BYTES + EXFAT_STREAM_NAME_LENGTH];
	uint32_t n = 0;
	for (uint32_t i = 2; i < entries && n < length; i++) {
		const uint8_t *const entry = set + i * EXFAT_ENTRY_BYTES;
		if (entry[0] != EXFAT_TYPE_NAME)
			continue;
		for (uint32_t j = 0; j < EXFAT_NAME_ENTRY_CHARS && n < length; j++, n++)
			name[n] = exfat_le16(entry + EXFAT_NAME_CHARS + 2 * j);
	}
	return n;
}

static void decode_set(const uint8_t *set, uint32_t entries, ExfatNode *node)
{
	const uint8_t *const file = set;
	const uint8_t *const stream = set + EXFAT_ENTRY_BYTES;
	memset(node, 0, sizeof(*node));
	node->attributes = exfat_le16(file + EXFAT_FILE_ATTRIBUTES);
	node->created = stamp_to_filetime(
		exfat_le32(file + EXFAT_FILE_CREATED), file[EXFAT_FILE_CREATED_10MS], file[EXFAT_FILE_CREATED_OFFSET]);
	node->modified = stamp_to_filetime(
		exfat_le32(file + EXFAT_FILE_MODIFIED), file[EXFAT_FILE_MODIFIED_10MS], file[EXFAT_FILE_MODIFIED_OFFSET]);
	node->accessed = stamp_to_filetime(exfat_le32(file + EXFAT_FILE_ACCESSED), 0, file[EXFAT_FILE_ACCESSED_OFFSET]);
	node->secondary_count = (uint8_t)(entries - 1);
	node->size_bytes = exfat_le64(stream + EXFAT_STREAM_DATA_LENGTH);
	node->valid_bytes = exfat_le64(stream + EXFAT_STREAM_VALID_LENGTH);
	node->first_cluster = exfat_le32(stream + EXFAT_STREAM_FIRST_CLUSTER);
	node->contiguous = node->first_cluster && (stream[EXFAT_STREAM_FLAGS] & EXFAT_STREAM_NO_FAT_CHAIN);
	if (node->valid_bytes > node->size_bytes)
		node->valid_bytes = node->size_bytes;
	uint16_t name[EXFAT_NAME_MAX];
	const uint32_t length = set_name(set, entries, name);
	for (uint32_t i = 0; i < length; i++)
		node->name[i] = name[i] < 0x100 ? (char)name[i] : '?';
	node->name[length] = 0;
}

static void put_stamp(uint8_t *file, uint32_t stamp_at, uint32_t increment_at, uint32_t offset_at, uint64_t filetime)
{
	uint32_t stamp;
	uint8_t increment;
	filetime_to_stamp(filetime, &stamp, &increment);
	exfat_put32(file + stamp_at, stamp);
	if (increment_at)
		file[increment_at] = increment;
	file[offset_at] = UTC_OFFSET_ZERO;
}

// The file and stream entries of a set from the node's fields.
static void encode_node(const ExfatNode *node, uint8_t *set)
{
	uint8_t *const file = set;
	uint8_t *const stream = set + EXFAT_ENTRY_BYTES;
	exfat_put16(file + EXFAT_FILE_ATTRIBUTES, node->attributes);
	put_stamp(file, EXFAT_FILE_CREATED, EXFAT_FILE_CREATED_10MS, EXFAT_FILE_CREATED_OFFSET, node->created);
	put_stamp(file, EXFAT_FILE_MODIFIED, EXFAT_FILE_MODIFIED_10MS, EXFAT_FILE_MODIFIED_OFFSET, node->modified);
	put_stamp(file, EXFAT_FILE_ACCESSED, 0, EXFAT_FILE_ACCESSED_OFFSET, node->accessed);
	stream[EXFAT_STREAM_FLAGS] = (uint8_t)(EXFAT_STREAM_ALLOCATION_POSSIBLE |
		(node->first_cluster && node->contiguous ? EXFAT_STREAM_NO_FAT_CHAIN : 0));
	exfat_put64(stream + EXFAT_STREAM_VALID_LENGTH, node->valid_bytes);
	exfat_put32(stream + EXFAT_STREAM_FIRST_CLUSTER, node->first_cluster);
	exfat_put64(stream + EXFAT_STREAM_DATA_LENGTH, node->size_bytes);
}

// A whole new set for the node under `name`; returns its entry count.
static uint32_t build_set(
	const ExfatVolume *v, const ExfatNode *node, const uint16_t *name, uint32_t length, uint8_t *set)
{
	const uint32_t entries = 2 + (length + EXFAT_NAME_ENTRY_CHARS - 1) / EXFAT_NAME_ENTRY_CHARS;
	memset(set, 0, entries * EXFAT_ENTRY_BYTES);
	set[0] = EXFAT_TYPE_FILE;
	set[EXFAT_FILE_SECONDARY_COUNT] = (uint8_t)(entries - 1);
	set[EXFAT_ENTRY_BYTES] = EXFAT_TYPE_STREAM;
	set[EXFAT_ENTRY_BYTES + EXFAT_STREAM_NAME_LENGTH] = (uint8_t)length;
	exfat_put16(set + EXFAT_ENTRY_BYTES + EXFAT_STREAM_NAME_HASH, name_hash(v, name, length));
	encode_node(node, set);
	for (uint32_t i = 0; i < length; i++) {
		uint8_t *const entry = set + (2 + i / EXFAT_NAME_ENTRY_CHARS) * EXFAT_ENTRY_BYTES;
		entry[0] = EXFAT_TYPE_NAME;
		exfat_put16(entry + EXFAT_NAME_CHARS + 2 * (i % EXFAT_NAME_ENTRY_CHARS), name[i]);
	}
	exfat_put16(set + EXFAT_FILE_CHECKSUM, exfat_checksum16(0, set, entries * EXFAT_ENTRY_BYTES, true));
	return entries;
}

static ExfatStatus write_entries(ExfatVolume *v, ExfatNode *dir, uint64_t offset, const uint8_t *set, uint32_t entries)
{
	for (uint32_t i = 0; i < entries; i++) {
		uint8_t *entry;
		const ExfatStatus status = directory_entry(v, dir, offset + i * EXFAT_ENTRY_BYTES, &entry);
		if (status != EXFAT_OK)
			return status == EXFAT_NOT_FOUND ? EXFAT_CORRUPT : status;
		memcpy(entry, set + i * EXFAT_ENTRY_BYTES, EXFAT_ENTRY_BYTES);
		cache_dirty(v, entry);
	}
	return EXFAT_OK;
}

// Writes a node's fields into its set on disk, keeping the name entries as they are.
static ExfatStatus write_back(ExfatVolume *v, const ExfatNode *node)
{
	if (!node->parent_cluster)
		return EXFAT_OK;
	ExfatNode dir = parent_of(node);
	uint8_t set[SET_BYTES_MAX];
	uint32_t entries;
	const ExfatStatus status = read_set(v, &dir, node->set_offset, set, &entries);
	if (status != EXFAT_OK)
		return status;
	encode_node(node, set);
	exfat_put16(set + EXFAT_FILE_CHECKSUM, exfat_checksum16(0, set, entries * EXFAT_ENTRY_BYTES, true));
	return write_entries(v, &dir, node->set_offset, set, 2);
}

static ExfatStatus mark_unused(ExfatVolume *v, const ExfatNode *node)
{
	ExfatNode dir = parent_of(node);
	for (uint32_t i = 0; i <= node->secondary_count; i++) {
		uint8_t *entry;
		const ExfatStatus status = directory_entry(v, &dir, node->set_offset + i * EXFAT_ENTRY_BYTES, &entry);
		if (status != EXFAT_OK)
			return status == EXFAT_NOT_FOUND ? EXFAT_CORRUPT : status;
		entry[0] &= (uint8_t)~EXFAT_TYPE_IN_USE;
		cache_dirty(v, entry);
	}
	return EXFAT_OK;
}

// Grows a directory by whole, zeroed clusters.
static ExfatStatus grow_directory(ExfatVolume *v, ExfatNode *dir, uint32_t clusters)
{
	const uint32_t have = node_clusters(v, dir);
	ExfatStatus status = extend(v, dir, have + clusters);
	if (status != EXFAT_OK)
		return status;
	dir->size_bytes += (uint64_t)clusters << v->cluster_shift;
	dir->valid_bytes = dir->size_bytes;
	for (uint32_t i = have; i < have + clusters; i++) {
		uint32_t cluster;
		uint32_t run;
		status = node_map(v, dir, i, &cluster, &run);
		if (status == EXFAT_OK)
			status = zero_cluster(v, cluster);
		if (status != EXFAT_OK)
			return status;
	}
	if (!dir->parent_cluster) {
		v->root_bytes = dir->size_bytes;
		return EXFAT_OK;
	}
	return write_back(v, dir);
}

// Where `count` entries in a row can go in a directory: unused ones, or the end, growing it when there is no room.
static ExfatStatus find_slot(ExfatVolume *v, ExfatNode *dir, uint32_t count, uint64_t *slot)
{
	uint64_t offset = 0;
	uint64_t run_start = 0;
	uint32_t run = 0;
	while (true) {
		uint8_t *entry;
		const ExfatStatus status = directory_entry(v, dir, offset, &entry);
		if (status != EXFAT_OK && status != EXFAT_NOT_FOUND)
			return status;
		if (status == EXFAT_NOT_FOUND || entry[0] == EXFAT_TYPE_END) {
			const uint64_t available = run + (dir->size_bytes - offset) / EXFAT_ENTRY_BYTES;
			*slot = offset - (uint64_t)run * EXFAT_ENTRY_BYTES;
			if (available >= count)
				return EXFAT_OK;
			const uint64_t missing_bytes = (count - available) * EXFAT_ENTRY_BYTES;
			return grow_directory(v, dir, clusters_for(v, missing_bytes));
		}
		if (entry[0] & EXFAT_TYPE_IN_USE) {
			run = 0;
		} else {
			if (!run)
				run_start = offset;
			if (++run == count) {
				*slot = run_start;
				return EXFAT_OK;
			}
		}
		offset += EXFAT_ENTRY_BYTES;
	}
}

// The set in `dir` named `name`, into `node`.
static ExfatStatus find_entry(ExfatVolume *v, ExfatNode *dir, const uint16_t *name, uint32_t length, ExfatNode *node)
{
	const uint16_t hash = name_hash(v, name, length);
	uint64_t offset = 0;
	while (true) {
		uint8_t *entry;
		ExfatStatus status = directory_entry(v, dir, offset, &entry);
		if (status != EXFAT_OK)
			return status;
		if (entry[0] == EXFAT_TYPE_END)
			return EXFAT_NOT_FOUND;
		if (entry[0] != EXFAT_TYPE_FILE) {
			offset += EXFAT_ENTRY_BYTES;
			continue;
		}
		const uint32_t count = 1u + entry[EXFAT_FILE_SECONDARY_COUNT];
		uint8_t set[SET_BYTES_MAX];
		uint32_t entries;
		status = read_set(v, dir, offset, set, &entries);
		if (status == EXFAT_CORRUPT) {
			offset += (uint64_t)count * EXFAT_ENTRY_BYTES;
			continue;
		}
		if (status != EXFAT_OK)
			return status;
		const uint8_t *const stream = set + EXFAT_ENTRY_BYTES;
		if (stream[EXFAT_STREAM_NAME_LENGTH] == length && exfat_le16(stream + EXFAT_STREAM_NAME_HASH) == hash) {
			uint16_t other[EXFAT_NAME_MAX];
			if (set_name(set, entries, other) == length && names_equal(v, name, other, length)) {
				decode_set(set, entries, node);
				node->parent_cluster = dir->first_cluster;
				node->parent_contiguous = dir->contiguous;
				node->set_offset = offset;
				return EXFAT_OK;
			}
		}
		offset += (uint64_t)entries * EXFAT_ENTRY_BYTES;
	}
}

static ExfatStatus lookup_length(ExfatVolume *v, const char *path, uint32_t length, ExfatNode *node)
{
	*node = root_node(v);
	uint32_t at = 0;
	while (at < length) {
		while (at < length && is_separator(path[at]))
			at++;
		if (at == length)
			break;
		uint32_t end = at;
		while (end < length && !is_separator(path[end]))
			end++;
		if (!(node->attributes & EXFAT_ATTRIBUTE_DIRECTORY))
			return EXFAT_NOT_DIRECTORY;
		uint16_t name[EXFAT_NAME_MAX];
		if (!component_name(path + at, end - at, name))
			return EXFAT_NAME;
		ExfatNode dir = *node;
		const ExfatStatus status = find_entry(v, &dir, name, end - at, node);
		if (status != EXFAT_OK)
			return status;
		at = end;
	}
	return EXFAT_OK;
}

// The parent directory and the UTF-16 name a path ends in; the name must not exist yet unless `existing` takes it.
static ExfatStatus resolve_new(
	ExfatVolume *v, const char *path, ExfatNode *dir, uint16_t *name, uint32_t *length, ExfatNode *existing)
{
	uint32_t parent_length;
	const char *leaf;
	if (!split_path(path, &parent_length, &leaf, length) || !component_name(leaf, *length, name))
		return EXFAT_NAME;
	ExfatStatus status = lookup_length(v, path, parent_length, dir);
	if (status != EXFAT_OK)
		return status;
	if (!(dir->attributes & EXFAT_ATTRIBUTE_DIRECTORY))
		return EXFAT_NOT_DIRECTORY;
	status = find_entry(v, dir, name, *length, existing);
	if (status == EXFAT_OK)
		return EXFAT_EXISTS;
	return status == EXFAT_NOT_FOUND ? EXFAT_OK : status;
}

static void set_name_latin1(ExfatNode *node, const uint16_t *name, uint32_t length)
{
	for (uint32_t i = 0; i < length; i++)
		node->name[i] = (char)name[i];
	node->name[length] = 0;
}

// Mounting.

static uint32_t log2_of(uint32_t value)
{
	uint32_t shift = 0;
	while ((1u << shift) < value)
		shift++;
	return shift;
}

static bool sector_bytes_valid(uint32_t bytes)
{
	return bytes >= 512 && bytes <= EXFAT_SECTOR_BYTES_MAX && (bytes & (bytes - 1)) == 0;
}

ExfatStatus xsdk_exfat_locate(ExfatDevice *device)
{
	uint8_t sector[EXFAT_SECTOR_BYTES_MAX];
	if (!sector_bytes_valid(device->sector_bytes))
		return EXFAT_UNSUPPORTED;
	if (device->read(device->context, 0, 1, sector))
		return EXFAT_IO;
	device->first_sector = 0;
	if (memcmp(sector + EXFAT_BOOT_NAME, "EXFAT   ", 8) == 0)
		return EXFAT_OK;
	if (sector[EXFAT_BOOT_SIGNATURE] != 0x55 || sector[EXFAT_BOOT_SIGNATURE + 1] != 0xAA)
		return EXFAT_NOT_EXFAT;
	for (unsigned i = 0; i < 4; i++) {
		const uint8_t *const entry = sector + MBR_PARTITION_TABLE + MBR_PARTITION_ENTRY_BYTES * i;
		if (entry[4] != MBR_TYPE_EXFAT)
			continue;
		const uint32_t start = exfat_le32(entry + 8);
		const uint32_t count = exfat_le32(entry + 12);
		if (!start || !count || (uint64_t)start + count > device->sectors)
			return EXFAT_CORRUPT;
		device->first_sector = start;
		device->sectors = count;
		return EXFAT_OK;
	}
	return EXFAT_NOT_EXFAT;
}

// Reads the boot sector's fields and checks them against each other and the device.
static ExfatStatus read_boot_sector(ExfatVolume *v)
{
	const uint8_t *const boot = v->bounce;
	ExfatStatus status = read_volume(v, 0, sector_bytes_of(v), v->bounce);
	if (status != EXFAT_OK)
		return status;
	if (memcmp(boot + EXFAT_BOOT_NAME, "EXFAT   ", 8) != 0 || boot[EXFAT_BOOT_SIGNATURE] != 0x55 ||
		boot[EXFAT_BOOT_SIGNATURE + 1] != 0xAA)
		return EXFAT_NOT_EXFAT;
	for (unsigned i = EXFAT_BOOT_NAME + 8; i < EXFAT_BOOT_PARTITION_OFFSET; i++) {
		if (boot[i])
			return EXFAT_NOT_EXFAT;
	}
	if (boot[EXFAT_BOOT_SECTOR_SHIFT] != v->sector_shift || boot[EXFAT_BOOT_FAT_COUNT] != 1)
		return EXFAT_UNSUPPORTED;
	if (boot[EXFAT_BOOT_REVISION + 1] != 1 || boot[EXFAT_BOOT_CLUSTER_SHIFT] > 25 - v->sector_shift)
		return EXFAT_CORRUPT;
	const uint64_t volume_sectors = exfat_le64(boot + EXFAT_BOOT_VOLUME_LENGTH);
	if (volume_sectors > v->device.sectors || volume_sectors < EXFAT_BOOT_SECTORS * 2)
		return EXFAT_CORRUPT;
	v->volume_bytes = volume_sectors << v->sector_shift;
	v->cluster_shift = v->sector_shift + boot[EXFAT_BOOT_CLUSTER_SHIFT];
	const uint64_t fat_sector = exfat_le32(boot + EXFAT_BOOT_FAT_OFFSET);
	const uint64_t fat_sectors = exfat_le32(boot + EXFAT_BOOT_FAT_LENGTH);
	const uint64_t heap_sector = exfat_le32(boot + EXFAT_BOOT_HEAP_OFFSET);
	v->cluster_count = exfat_le32(boot + EXFAT_BOOT_CLUSTER_COUNT);
	v->root_cluster = exfat_le32(boot + EXFAT_BOOT_ROOT_CLUSTER);
	v->serial = exfat_le32(boot + EXFAT_BOOT_SERIAL);
	v->dirty = (exfat_le16(boot + EXFAT_BOOT_FLAGS) & EXFAT_FLAG_DIRTY) != 0;
	if (fat_sector < EXFAT_BOOT_SECTORS * 2 || heap_sector < fat_sector + fat_sectors ||
		((uint64_t)v->cluster_count + 2) * 4 > fat_sectors << v->sector_shift ||
		heap_sector + ((uint64_t)v->cluster_count << boot[EXFAT_BOOT_CLUSTER_SHIFT]) > volume_sectors ||
		!cluster_valid(v, v->root_cluster))
		return EXFAT_CORRUPT;
	v->fat_offset = fat_sector << v->sector_shift;
	v->fat_bytes = fat_sectors << v->sector_shift;
	v->heap_offset = heap_sector << v->sector_shift;
	return EXFAT_OK;
}

// The checksum sector against the eleven before it, read into the still-empty cache's memory.
static ExfatStatus check_boot_region(ExfatVolume *v)
{
	const uint32_t sector_bytes = sector_bytes_of(v);
	uint8_t *const region = v->blocks[0];
	const ExfatStatus status = read_volume(v, 0, EXFAT_BOOT_SECTORS * sector_bytes, region);
	if (status != EXFAT_OK)
		return status;
	const uint32_t checksum = exfat_checksum32(0, region, (EXFAT_BOOT_SECTORS - 1) * sector_bytes, true);
	return checksum == exfat_le32(region + (EXFAT_BOOT_SECTORS - 1) * sector_bytes) ? EXFAT_OK : EXFAT_CORRUPT;
}

static ExfatStatus measure_root(ExfatVolume *v)
{
	uint32_t cluster = v->root_cluster;
	uint32_t count = 0;
	while (cluster != EXFAT_CLUSTER_END) {
		if (!cluster_valid(v, cluster) || ++count > v->cluster_count)
			return EXFAT_CORRUPT;
		const ExfatStatus status = fat_read(v, cluster, &cluster);
		if (status != EXFAT_OK)
			return status;
	}
	v->root_bytes = (uint64_t)count << v->cluster_shift;
	return EXFAT_OK;
}

// The up-case table, through the cache, and its checksum against the entry's.
static ExfatStatus load_upcase(ExfatVolume *v, uint32_t cluster, uint64_t bytes, uint32_t checksum)
{
	if (bytes > EXFAT_UPCASE_BYTES_MAX)
		return EXFAT_UNSUPPORTED;
	if (!cluster_valid(v, cluster) || bytes < 2 || bytes % 2)
		return EXFAT_CORRUPT;
	uint8_t *const table = (uint8_t *)v->upcase;
	const uint64_t offset = cluster_offset(v, cluster);
	for (uint32_t done = 0; done < bytes;) {
		uint8_t *piece;
		const ExfatStatus status = cached(v, offset + done, &piece);
		if (status != EXFAT_OK)
			return status;
		uint32_t count = EXFAT_CACHE_BLOCK_BYTES - (uint32_t)((offset + done) % EXFAT_CACHE_BLOCK_BYTES);
		if (count > bytes - done)
			count = (uint32_t)(bytes - done);
		memcpy(table + done, piece, count);
		done += count;
	}
	if (exfat_checksum32(0, table, (uint32_t)bytes, false) != checksum)
		return EXFAT_CORRUPT;
	for (uint32_t i = 0; i < bytes / 2; i++)
		v->upcase[i] = exfat_le16(table + 2 * i);
	v->upcase_words = (uint32_t)(bytes / 2);
	return EXFAT_OK;
}

// The bitmap, up-case and label entries of the root directory.
static ExfatStatus read_root_entries(ExfatVolume *v)
{
	ExfatNode root = root_node(v);
	uint32_t upcase_cluster = 0;
	uint64_t upcase_bytes = 0;
	uint32_t upcase_checksum = 0;
	for (uint64_t offset = 0;; offset += EXFAT_ENTRY_BYTES) {
		uint8_t *entry;
		const ExfatStatus status = directory_entry(v, &root, offset, &entry);
		if (status == EXFAT_NOT_FOUND)
			break;
		if (status != EXFAT_OK)
			return status;
		if (entry[0] == EXFAT_TYPE_END)
			break;
		if (entry[0] == EXFAT_TYPE_BITMAP && !v->bitmap_cluster) {
			v->bitmap_cluster = exfat_le32(entry + EXFAT_ALLOCATION_FIRST_CLUSTER);
			v->bitmap_bytes = exfat_le64(entry + EXFAT_ALLOCATION_DATA_LENGTH);
		} else if (entry[0] == EXFAT_TYPE_UPCASE && !upcase_cluster) {
			upcase_cluster = exfat_le32(entry + EXFAT_ALLOCATION_FIRST_CLUSTER);
			upcase_bytes = exfat_le64(entry + EXFAT_ALLOCATION_DATA_LENGTH);
			upcase_checksum = exfat_le32(entry + EXFAT_UPCASE_CHECKSUM);
		} else if (entry[0] == EXFAT_TYPE_LABEL) {
			const uint32_t count =
				entry[EXFAT_LABEL_COUNT] < EXFAT_LABEL_MAX ? entry[EXFAT_LABEL_COUNT] : EXFAT_LABEL_MAX;
			for (uint32_t i = 0; i < count; i++) {
				const uint16_t c = exfat_le16(entry + EXFAT_LABEL_TEXT + 2 * i);
				v->label[i] = c < 0x100 ? (char)c : '?';
			}
			v->label[count] = 0;
		}
	}
	if (!v->bitmap_cluster || !upcase_cluster || !cluster_valid(v, v->bitmap_cluster))
		return EXFAT_CORRUPT;
	if (v->bitmap_bytes < ((uint64_t)v->cluster_count + 7) / 8 ||
		cluster_offset(v, v->bitmap_cluster) + v->bitmap_bytes > v->volume_bytes)
		return EXFAT_CORRUPT;
	return load_upcase(v, upcase_cluster, upcase_bytes, upcase_checksum);
}

static ExfatStatus count_free_clusters(ExfatVolume *v)
{
	v->free_clusters = 0;
	for (uint32_t cluster = EXFAT_FIRST_CLUSTER; cluster < EXFAT_FIRST_CLUSTER + v->cluster_count; cluster += 8) {
		uint8_t *byte;
		const ExfatStatus status = bitmap_byte(v, cluster, &byte);
		if (status != EXFAT_OK)
			return status;
		uint32_t bits = EXFAT_FIRST_CLUSTER + v->cluster_count - cluster;
		if (bits > 8)
			bits = 8;
		for (uint32_t bit = 0; bit < bits; bit++)
			v->free_clusters += !(*byte >> bit & 1);
	}
	return EXFAT_OK;
}

ExfatStatus xsdk_exfat_mount(ExfatVolume *v, const ExfatDevice *device)
{
	memset(v, 0, sizeof(*v));
	v->device = *device;
	if (!sector_bytes_valid(device->sector_bytes) || !device->sectors)
		return EXFAT_UNSUPPORTED;
	v->sector_shift = log2_of(device->sector_bytes);
	v->volume_bytes = device->sectors << v->sector_shift;
	ExfatStatus status = read_boot_sector(v);
	if (status == EXFAT_OK)
		status = check_boot_region(v);
	if (status != EXFAT_OK)
		return status;
	for (uint32_t i = 0; i < EXFAT_CACHE_BLOCKS; i++)
		v->slots[i].block = UINT64_MAX;
	status = measure_root(v);
	if (status == EXFAT_OK)
		status = read_root_entries(v);
	if (status == EXFAT_OK)
		status = count_free_clusters(v);
	v->next_free = EXFAT_FIRST_CLUSTER;
	return status;
}

ExfatStatus xsdk_exfat_sync(ExfatVolume *v)
{
	const ExfatStatus status = cache_flush(v);
	if (status != EXFAT_OK)
		return status;
	return v->dirtied ? set_dirty_flag(v, false) : EXFAT_OK;
}

ExfatStatus xsdk_exfat_unmount(ExfatVolume *v)
{
	return xsdk_exfat_sync(v);
}

const char *xsdk_exfat_status_name(ExfatStatus status)
{
	return (unsigned)status < sizeof(status_names) / sizeof(status_names[0]) ? status_names[status] : "?";
}

// Files and directories.

ExfatStatus xsdk_exfat_lookup(ExfatVolume *v, const char *path, ExfatNode *node)
{
	return lookup_length(v, path, (uint32_t)strlen(path), node);
}

uint16_t xsdk_exfat_upcase(const ExfatVolume *v, uint16_t c)
{
	return upcase_char(v, c);
}

ExfatStatus xsdk_exfat_open_directory(const ExfatVolume *v, const ExfatNode *directory, ExfatDirectory *listing)
{
	(void)v;
	if (!(directory->attributes & EXFAT_ATTRIBUTE_DIRECTORY))
		return EXFAT_NOT_DIRECTORY;
	listing->node = *directory;
	listing->offset = 0;
	return EXFAT_OK;
}

ExfatStatus xsdk_exfat_read_directory(ExfatVolume *v, ExfatDirectory *listing, ExfatNode *entry_node)
{
	while (true) {
		uint8_t *entry;
		ExfatStatus status = directory_entry(v, &listing->node, listing->offset, &entry);
		if (status != EXFAT_OK)
			return status;
		if (entry[0] == EXFAT_TYPE_END)
			return EXFAT_NOT_FOUND;
		if (entry[0] != EXFAT_TYPE_FILE) {
			listing->offset += EXFAT_ENTRY_BYTES;
			continue;
		}
		const uint32_t count = 1u + entry[EXFAT_FILE_SECONDARY_COUNT];
		uint8_t set[SET_BYTES_MAX];
		uint32_t entries;
		status = read_set(v, &listing->node, listing->offset, set, &entries);
		if (status == EXFAT_CORRUPT) {
			listing->offset += (uint64_t)count * EXFAT_ENTRY_BYTES;
			continue;
		}
		if (status != EXFAT_OK)
			return status;
		decode_set(set, entries, entry_node);
		entry_node->parent_cluster = listing->node.first_cluster;
		entry_node->parent_contiguous = listing->node.contiguous;
		entry_node->set_offset = listing->offset;
		listing->offset += (uint64_t)entries * EXFAT_ENTRY_BYTES;
		return EXFAT_OK;
	}
}

ExfatStatus xsdk_exfat_read(
	ExfatVolume *v, ExfatNode *file, uint64_t offset, void *buffer, uint32_t bytes, uint32_t *read_bytes)
{
	*read_bytes = 0;
	if (file->attributes & EXFAT_ATTRIBUTE_DIRECTORY)
		return EXFAT_IS_DIRECTORY;
	if (offset >= file->size_bytes || !bytes)
		return EXFAT_OK;
	if (bytes > file->size_bytes - offset)
		bytes = (uint32_t)(file->size_bytes - offset);
	uint32_t from_disk = 0;
	if (offset < file->valid_bytes)
		from_disk = bytes < file->valid_bytes - offset ? bytes : (uint32_t)(file->valid_bytes - offset);
	if (from_disk) {
		const ExfatStatus status = transfer(v, file, offset, buffer, from_disk, false);
		if (status != EXFAT_OK)
			return status;
	}
	memset((uint8_t *)buffer + from_disk, 0, bytes - from_disk);
	*read_bytes = bytes;
	return EXFAT_OK;
}

ExfatStatus xsdk_exfat_write(ExfatVolume *v, ExfatNode *file, uint64_t offset, const void *buffer, uint32_t bytes)
{
	if (file->attributes & EXFAT_ATTRIBUTE_DIRECTORY)
		return EXFAT_IS_DIRECTORY;
	if (!file->parent_cluster)
		return EXFAT_NAME;
	if (!bytes)
		return EXFAT_OK;
	const uint64_t end = offset + bytes;
	ExfatStatus status = set_dirty_flag(v, true);
	if (status == EXFAT_OK && end > file->size_bytes) {
		status = extend(v, file, clusters_for(v, end));
		// The node's size says how many clusters it has; the mapping below needs the new ones.
		if (status == EXFAT_OK)
			file->size_bytes = end;
	}
	if (status == EXFAT_OK && offset > file->valid_bytes)
		status = transfer(v, file, file->valid_bytes, NULL, offset - file->valid_bytes, true);
	if (status == EXFAT_OK)
		status = transfer(v, file, offset, (uint8_t *)(uintptr_t)buffer, bytes, true);
	if (status != EXFAT_OK)
		return status;
	if (end > file->valid_bytes)
		file->valid_bytes = end;
	file->modified = now(v);
	file->attributes |= EXFAT_ATTRIBUTE_ARCHIVE;
	status = write_back(v, file);
	return status == EXFAT_OK ? settle(v) : status;
}

ExfatStatus xsdk_exfat_resize(ExfatVolume *v, ExfatNode *file, uint64_t size_bytes)
{
	if (file->attributes & EXFAT_ATTRIBUTE_DIRECTORY)
		return EXFAT_IS_DIRECTORY;
	if (!file->parent_cluster)
		return EXFAT_NAME;
	if (size_bytes == file->size_bytes)
		return EXFAT_OK;
	ExfatStatus status = set_dirty_flag(v, true);
	if (status != EXFAT_OK)
		return status;
	const uint32_t have = node_clusters(v, file);
	const uint32_t needed = clusters_for(v, size_bytes);
	if (size_bytes > file->size_bytes) {
		status = extend(v, file, needed);
		if (status != EXFAT_OK)
			return status;
		file->size_bytes = size_bytes;
		file->modified = now(v);
		status = write_back(v, file);
		return status == EXFAT_OK ? settle(v) : status;
	}
	// The smaller set goes to disk before its clusters are freed.
	ExfatNode shrunk = *file;
	shrunk.size_bytes = size_bytes;
	if (shrunk.valid_bytes > size_bytes)
		shrunk.valid_bytes = size_bytes;
	shrunk.modified = now(v);
	if (!needed) {
		shrunk.first_cluster = 0;
		shrunk.contiguous = false;
	}
	status = write_back(v, &shrunk);
	if (status == EXFAT_OK)
		status = cache_flush(v);
	if (status == EXFAT_OK)
		status = release(v, file, have, needed);
	if (status != EXFAT_OK)
		return status;
	shrunk.walk_index = 0;
	shrunk.walk_cluster = 0;
	*file = shrunk;
	return settle(v);
}

ExfatStatus xsdk_exfat_update(ExfatVolume *v, const ExfatNode *node)
{
	if (!node->parent_cluster)
		return EXFAT_NAME;
	ExfatStatus status = set_dirty_flag(v, true);
	if (status == EXFAT_OK)
		status = write_back(v, node);
	return status == EXFAT_OK ? settle(v) : status;
}

ExfatStatus xsdk_exfat_create(ExfatVolume *v, const char *path, bool directory, ExfatNode *node)
{
	ExfatNode dir;
	uint16_t name[EXFAT_NAME_MAX];
	uint32_t length;
	ExfatNode existing;
	ExfatStatus status = resolve_new(v, path, &dir, name, &length, &existing);
	if (status != EXFAT_OK)
		return status;
	status = set_dirty_flag(v, true);
	if (status != EXFAT_OK)
		return status;
	const uint32_t entries = 2 + (length + EXFAT_NAME_ENTRY_CHARS - 1) / EXFAT_NAME_ENTRY_CHARS;
	uint64_t slot;
	status = find_slot(v, &dir, entries, &slot);
	if (status != EXFAT_OK)
		return status;
	memset(node, 0, sizeof(*node));
	set_name_latin1(node, name, length);
	node->attributes = directory ? EXFAT_ATTRIBUTE_DIRECTORY : EXFAT_ATTRIBUTE_ARCHIVE;
	node->created = now(v);
	node->modified = node->created;
	node->accessed = node->created;
	if (directory) {
		status = extend(v, node, 1);
		if (status == EXFAT_OK)
			status = zero_cluster(v, node->first_cluster);
		if (status != EXFAT_OK)
			return status;
		node->size_bytes = cluster_bytes_of(v);
		node->valid_bytes = node->size_bytes;
	}
	node->parent_cluster = dir.first_cluster;
	node->parent_contiguous = dir.contiguous;
	node->set_offset = slot;
	node->secondary_count = (uint8_t)(entries - 1);
	uint8_t set[SET_BYTES_MAX];
	build_set(v, node, name, length, set);
	status = write_entries(v, &dir, slot, set, entries);
	return status == EXFAT_OK ? settle(v) : status;
}

ExfatStatus xsdk_exfat_remove(ExfatVolume *v, const ExfatNode *node)
{
	if (!node->parent_cluster)
		return EXFAT_NAME;
	const bool directory = (node->attributes & EXFAT_ATTRIBUTE_DIRECTORY) != 0;
	ExfatStatus status;
	if (directory) {
		ExfatDirectory listing;
		ExfatNode child;
		status = xsdk_exfat_open_directory(v, node, &listing);
		if (status == EXFAT_OK)
			status = xsdk_exfat_read_directory(v, &listing, &child);
		if (status == EXFAT_OK)
			return EXFAT_NOT_EMPTY;
		if (status != EXFAT_NOT_FOUND)
			return status;
	}
	status = set_dirty_flag(v, true);
	if (status == EXFAT_OK)
		status = mark_unused(v, node);
	if (status == EXFAT_OK)
		status = cache_flush(v);
	if (status != EXFAT_OK)
		return status;
	ExfatNode gone = *node;
	const uint32_t have = node_clusters(v, &gone);
	for (uint32_t i = 0; directory && i < have; i++) {
		uint32_t cluster;
		uint32_t run;
		status = node_map(v, &gone, i, &cluster, &run);
		if (status != EXFAT_OK)
			return status;
		cache_drop(v, cluster_offset(v, cluster), cluster_bytes_of(v));
	}
	status = release(v, &gone, have, 0);
	return status == EXFAT_OK ? settle(v) : status;
}

// Whether a directory is, or lies under, the node: moving it there would cut it loose.
static ExfatStatus is_inside(ExfatVolume *v, const ExfatNode *node, const char *path, uint32_t length, bool *inside)
{
	*inside = false;
	for (uint32_t end = 0; end <= length; end++) {
		if (end < length && !is_separator(path[end]))
			continue;
		ExfatNode prefix;
		const ExfatStatus status = lookup_length(v, path, end, &prefix);
		if (status != EXFAT_OK)
			return status;
		if (prefix.first_cluster == node->first_cluster) {
			*inside = true;
			return EXFAT_OK;
		}
	}
	return EXFAT_OK;
}

ExfatStatus xsdk_exfat_rename(ExfatVolume *v, ExfatNode *node, const char *new_path)
{
	if (!node->parent_cluster)
		return EXFAT_NAME;
	ExfatNode dir;
	uint16_t name[EXFAT_NAME_MAX];
	uint32_t length;
	ExfatNode existing;
	ExfatStatus status = resolve_new(v, new_path, &dir, name, &length, &existing);
	if (status == EXFAT_EXISTS &&
		(existing.parent_cluster != node->parent_cluster || existing.set_offset != node->set_offset))
		return EXFAT_EXISTS;
	if (status != EXFAT_OK && status != EXFAT_EXISTS)
		return status;
	if (node->attributes & EXFAT_ATTRIBUTE_DIRECTORY) {
		uint32_t parent_length;
		const char *leaf;
		uint32_t leaf_length;
		split_path(new_path, &parent_length, &leaf, &leaf_length);
		bool inside;
		status = is_inside(v, node, new_path, parent_length, &inside);
		if (status != EXFAT_OK)
			return status;
		if (inside)
			return EXFAT_NAME;
	}
	status = set_dirty_flag(v, true);
	if (status != EXFAT_OK)
		return status;
	const uint32_t entries = 2 + (length + EXFAT_NAME_ENTRY_CHARS - 1) / EXFAT_NAME_ENTRY_CHARS;
	uint64_t slot;
	status = find_slot(v, &dir, entries, &slot);
	if (status != EXFAT_OK)
		return status;
	ExfatNode moved = *node;
	set_name_latin1(&moved, name, length);
	moved.parent_cluster = dir.first_cluster;
	moved.parent_contiguous = dir.contiguous;
	moved.set_offset = slot;
	moved.secondary_count = (uint8_t)(entries - 1);
	uint8_t set[SET_BYTES_MAX];
	build_set(v, &moved, name, length, set);
	// The new set reaches the disk before the old one goes.
	status = write_entries(v, &dir, slot, set, entries);
	if (status == EXFAT_OK)
		status = cache_flush(v);
	if (status == EXFAT_OK)
		status = mark_unused(v, node);
	if (status != EXFAT_OK)
		return status;
	*node = moved;
	return settle(v);
}
