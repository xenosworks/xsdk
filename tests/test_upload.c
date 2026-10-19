#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tests.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/texture.h"

// Tiled uploads (src/texture.c). From word-aligned rows, whole tiles go at once, their offsets worked out a tile at a
// time; every case must come out byte for byte as a block at a time leaves it, from the same rows a byte off word
// alignment. Then 512x512 uploads from cached memory, timed.

enum {
	// Both textures' words hold this first, so a byte written where it shouldn't be shows.
	SENTINEL = 0xA5C3E1F0,
	// Rows a word longer than their blocks, so they start at different alignments within 16 bytes.
	ROW_PAD_BYTES = 4,
	// Enough for any case's level or slice: R8 520x37 takes 524 bytes a row, padded, by 37 rows.
	MAX_SOURCE_BYTES = 20 * 1024,
	MAX_MISSES_LOGGED = 8,
	TIMED_SIZE = 512,
};

typedef struct {
	const char *name;
	uint32_t dimension;
	uint32_t format;
	uint32_t width;
	uint32_t height;
	// A 3D texture's slices; a cube has 6 faces.
	uint32_t depth;
	uint32_t levels;
	// Bytes in a texel, or in a 4 by 4 block.
	uint32_t block_bytes;
	uint32_t block_log2;
} UploadCase;

// Every block size, 2D, cube and 3D, every level of some, and rows of whole tiles with blocks over: 1 and 2-byte blocks
// cross tiles whose offsets carry over a bit between the tile bits (xsdk_tiled_tile_bits).
static const UploadCase cases[] = {
	{ "R8 520x37", XSDK_TEXTURE_2D, XSDK_FORMAT_R8, 520, 37, 1, 1, 1, 0 },
	{ "R8 512x16, every level", XSDK_TEXTURE_2D, XSDK_FORMAT_R8, 512, 16, 1, 10, 1, 0 },
	{ "R5G6B5 200x33, every level", XSDK_TEXTURE_2D, XSDK_FORMAT_R5G6B5, 200, 33, 1, 8, 2, 0 },
	{ "A8R8G8B8 100x37", XSDK_TEXTURE_2D, XSDK_FORMAT_A8R8G8B8, 100, 37, 1, 1, 4, 0 },
	{ "A8R8G8B8 cube 40, every level", XSDK_TEXTURE_CUBE, XSDK_FORMAT_A8R8G8B8, 40, 40, 6, 6, 4, 0 },
	{ "R16G16B16A16F 70x9", XSDK_TEXTURE_2D, XSDK_FORMAT_R16G16B16A16F, 70, 9, 1, 1, 8, 0 },
	{ "DXT1 520x20", XSDK_TEXTURE_2D, XSDK_FORMAT_DXT1, 520, 20, 1, 1, 8, 2 },
	{ "DXT5 136x64, every level", XSDK_TEXTURE_2D, XSDK_FORMAT_DXT5, 136, 64, 1, 8, 16, 2 },
	{ "R8 3D 130x20x5", XSDK_TEXTURE_3D, XSDK_FORMAT_R8, 130, 20, 5, 1, 1, 0 },
	{ "R5G6B5 3D 100x33x3", XSDK_TEXTURE_3D, XSDK_FORMAT_R5G6B5, 100, 33, 3, 1, 2, 0 },
	{ "R32F 3D 40x40x6", XSDK_TEXTURE_3D, XSDK_FORMAT_R32F, 40, 40, 6, 1, 4, 0 },
};

typedef struct {
	const char *check_name;
	const char *format_name;
	uint32_t format;
	uint32_t texel_bytes;
	// A third of what runs took before whole tiles: 2226 us for R8 on the console (docs/HARDWARE.md, "Uploads in runs,
	// and channel swizzles"), and twice that for 32-bit texels, twice the runs at the same cost each.
	uint32_t limit_us;
} TimedUpload;

static const TimedUpload timed[] = {
	{ "uploads: a 512x512 R8 tiled upload takes under 750 us", "R8", XSDK_FORMAT_R8, 1, 750 },
	{ "uploads: a 512x512 A8R8G8B8 tiled upload takes under 1500 us", "A8R8G8B8", XSDK_FORMAT_A8R8G8B8, 4, 1500 },
};

// The same rows twice: word-aligned, and a byte in, which xsdk_texture_upload takes a block at a time.
static uint8_t aligned_rows[MAX_SOURCE_BYTES] __attribute__((aligned(16)));
static uint8_t unaligned_rows[MAX_SOURCE_BYTES + 1];

static uint32_t level_size(uint32_t size, uint32_t level)
{
	return size >> level ? size >> level : 1;
}

static uint32_t blocks(uint32_t texels, uint32_t block_log2)
{
	return (texels + (1u << block_log2) - 1) >> block_log2;
}

static bool make_texture(XsdkTexture *texture, const UploadCase *c)
{
	NTSTATUS status;
	if (c->dimension == XSDK_TEXTURE_3D)
		status = xsdk_texture_init_3d(texture, c->format, c->width, c->height, c->depth);
	else if (c->dimension == XSDK_TEXTURE_CUBE)
		status = xsdk_texture_init_cube(texture, c->format, c->width, c->levels);
	else
		status = xsdk_texture_init(texture, c->format, c->width, c->height, c->levels, true);
	volatile uint32_t *const texels = NT_SUCCESS(status) ? xsdk_gpu_alloc(texture->size_bytes) : NULL;
	if (!texels) {
		xsdk_log_write("uploads: %s not made: 0x%08X", c->name, (unsigned)status);
		return false;
	}
	xsdk_texture_attach(texture, texels);
	for (uint32_t i = 0; i < texture->size_bytes / 4; i++)
		texels[i] = SENTINEL;
	return true;
}

// A level's face or slice, its first third of rows in one upload and the rest in another, into both textures.
static bool upload_both(XsdkTexture *tiles, XsdkTexture *blocks_alone, const UploadCase *c, uint32_t level,
	uint32_t slice, uint32_t columns, uint32_t rows)
{
	const uint32_t row_bytes = (columns * c->block_bytes + 3) / 4 * 4 + ROW_PAD_BYTES;
	if (row_bytes * rows > MAX_SOURCE_BYTES) {
		xsdk_log_write("uploads: %s: level %u is past the source rows", c->name, (unsigned)level);
		return false;
	}
	for (uint32_t i = 0; i < row_bytes * rows; i++)
		aligned_rows[i] = (uint8_t)((i * 2654435761u + level * 31 + slice * 7) >> 13);
	memcpy(unaligned_rows + 1, aligned_rows, row_bytes * rows);
	const uint32_t cut = rows / 3;
	xsdk_texture_upload_slice_rows(tiles, level, slice, 0, cut, aligned_rows, row_bytes);
	xsdk_texture_upload_slice_rows(tiles, level, slice, cut, rows - cut, aligned_rows + cut * row_bytes, row_bytes);
	xsdk_texture_upload_slice_rows(blocks_alone, level, slice, 0, cut, unaligned_rows + 1, row_bytes);
	xsdk_texture_upload_slice_rows(
		blocks_alone, level, slice, cut, rows - cut, unaligned_rows + 1 + cut * row_bytes, row_bytes);
	return true;
}

// The words of the two textures that differ, the first few logged.
static uint32_t count_differences(const XsdkTexture *tiles, const XsdkTexture *blocks_alone, const UploadCase *c)
{
	uint32_t wrong = 0;
	for (uint32_t i = 0; i < tiles->size_bytes / 4; i++) {
		const uint32_t got = tiles->texels[i];
		const uint32_t want = blocks_alone->texels[i];
		if (got != want && wrong++ < MAX_MISSES_LOGGED)
			xsdk_log_write("uploads: %s: byte %u holds 0x%08X, a block at a time 0x%08X", c->name, (unsigned)(i * 4),
				(unsigned)got, (unsigned)want);
	}
	return wrong;
}

static bool upload_matches(const UploadCase *c)
{
	XsdkTexture tiles = { .texels = NULL };
	XsdkTexture blocks_alone = { .texels = NULL };
	bool same = make_texture(&tiles, c) && make_texture(&blocks_alone, c);
	const uint32_t slices = c->dimension == XSDK_TEXTURE_2D ? 1 : c->depth;
	for (uint32_t level = 0; same && level < c->levels; level++) {
		const uint32_t columns = blocks(level_size(c->width, level), c->block_log2);
		const uint32_t rows = blocks(level_size(c->height, level), c->block_log2);
		for (uint32_t slice = 0; same && slice < slices; slice++)
			same = upload_both(&tiles, &blocks_alone, c, level, slice, columns, rows);
	}
	if (same) {
		const uint32_t wrong = count_differences(&tiles, &blocks_alone, c);
		if (wrong)
			xsdk_log_write(
				"uploads: %s: %u of %u words differ", c->name, (unsigned)wrong, (unsigned)(tiles.size_bytes / 4));
		same = wrong == 0;
	}
	if (tiles.texels)
		xsdk_texture_free(&tiles);
	if (blocks_alone.texels)
		xsdk_texture_free(&blocks_alone);
	return same;
}

static int64_t now_us(void)
{
	struct timespec time;
	(void)clock_gettime(CLOCK_MONOTONIC, &time);
	return (int64_t)time.tv_sec * 1000000 + time.tv_nsec / 1000;
}

static void check_upload_time(const TimedUpload *t)
{
	XsdkTexture texture;
	const uint32_t size_bytes = TIMED_SIZE * TIMED_SIZE * t->texel_bytes;
	const NTSTATUS status = xsdk_texture_create(&texture, t->format, TIMED_SIZE, TIMED_SIZE, 1, true);
	uint8_t *const data = malloc(size_bytes);
	if (!NT_SUCCESS(status) || !data) {
		check(t->check_name, false);
		free(data);
		if (NT_SUCCESS(status))
			xsdk_texture_free(&texture);
		return;
	}
	for (uint32_t i = 0; i < size_bytes; i++)
		data[i] = (uint8_t)(i * 7);
	const int64_t start = now_us();
	xsdk_texture_upload(&texture, 0, data, TIMED_SIZE * t->texel_bytes);
	const int64_t elapsed = now_us() - start;
	xsdk_log_write("uploads: a 512x512 %s tiled upload took %u us, %u MB/s", t->format_name, (unsigned)elapsed,
		(unsigned)(elapsed > 0 ? (int64_t)size_bytes / elapsed : 0));
	check(t->check_name, elapsed < t->limit_us);
	free(data);
	xsdk_texture_free(&texture);
}

void test_upload(void)
{
	bool same = true;
	for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
		same = upload_matches(&cases[i]) && same;
	check("uploads: tiled uploads from word-aligned rows write what a block at a time does, byte for byte", same);
	for (uint32_t i = 0; i < sizeof timed / sizeof timed[0]; i++)
		check_upload_time(&timed[i]);
}
