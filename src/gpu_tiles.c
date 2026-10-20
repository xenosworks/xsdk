#include "xsdk/gpu.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "gpu_private.h"
#include "xsdk/log.h"

// Tiled passes at 1, 2 and 4 samples (docs/scope/44-tiling-and-msaa.md, "Tiled passes in xsdk"), on what the console
// showed of the window offset, the moved resolve and depth, and predication (docs/HARDWARE.md, "Tiling's registers and
// packets, milestone 44 step 1").

enum {
	// The window offset's and the scissors' y from bit 16, the offset 15 bits signed
	// (xenia:src/xenia/gpu/registers.h:586-633).
	Y_SHIFT = 16,
	OFFSET_MASK = 0x7FFF,
	// Inside the window offset's 15 bits and the 8192-texel textures (xenia:src/xenia/gpu/xenos.h:1139-1141).
	MAX_SIDE = 8192,
	// D3D's screen scissor, which xsdk's set-up leaves (src/gpu_setup.c).
	SCREEN_WHOLE = 8192,
	// A bin register's two words, sent high first as Xenia reads them; the console gave the same either way round.
	BIN_DWORDS = 3,
	WINDOW_DWORDS = 2 + 3,
	// A pass's colour targets and its depth, each resolved once.
	RESOLVES_KEPT = XSDK_COLOR_TARGETS + 1,
};

static const uint64_t ALL_BINS = UINT64_MAX;

// The pass being recorded or last queued: how its bands and targets lie, and whether a draw narrowed its bins.
static bool tiling;
static XsdkTiling pass;
static XsdkCommandBuffer *pass_buffer;
static uint32_t band_rows;
static XsdkColorTarget placed[XSDK_COLOR_TARGETS];
static bool narrowed;
// The pass whose last band's resolves left the band clear, to that depth and those colours, while nothing has changed
// eDRAM since; none kept if it had more resolves than a target each.
static XsdkTiling cleared;
static uint32_t cleared_rows;
static uint32_t cleared_depth;
static XsdkTileResolve cleared_resolves[RESOLVES_KEPT];
static uint32_t cleared_count;
static bool band_clear;

bool xsdk_gpu_tiling(void)
{
	return tiling;
}

void xsdk_gpu_tiles_forget(void)
{
	band_clear = false;
}

// A band of rows: its colour targets and its depth, or the whole depth that stays.
static uint32_t band_tiles(const XsdkTiling *t, uint32_t rows)
{
	const uint32_t depth_rows = t->depth_whole ? t->height : rows;
	uint32_t tiles = t->depth ? xsdk_gpu_target_sample_tiles(t->width, depth_rows, t->samples) : 0;
	for (uint32_t i = 0; i < t->count; i++)
		tiles += xsdk_gpu_color_target_sample_tiles(t->width, rows, t->targets[i].format, t->samples);
	return tiles;
}

// As few bands as fit, each as many rows, rounded up to a multiple of XSDK_TILE_ROWS; 0 if none fit.
static uint32_t rows_of(const XsdkTiling *t)
{
	if (t->band_rows)
		return t->band_rows;
	for (uint32_t bands = 1; bands <= XSDK_TILE_BANDS; bands++) {
		const uint32_t rows = ((t->height + bands - 1) / bands + XSDK_TILE_ROWS - 1) / XSDK_TILE_ROWS * XSDK_TILE_ROWS;
		if (band_tiles(t, rows) <= XSDK_EDRAM_TILES)
			return rows;
	}
	return 0;
}

// 1, 2 or 4 samples, each colour target's format taking them.
static bool takes_samples(const XsdkTiling *t)
{
	bool takes = t->samples == 1 || t->samples == 2 || t->samples == 4;
	for (uint32_t i = 0; takes && i < t->count; i++)
		takes = xsdk_gpu_target_takes_samples(t->targets[i].format, t->samples);
	return takes;
}

uint32_t xsdk_gpu_tiling_bands(const XsdkTiling *t)
{
	const bool valid = t->width && t->width <= MAX_SIDE && t->height && t->height <= MAX_SIDE &&
		t->count <= XSDK_COLOR_TARGETS && takes_samples(t) && (t->count || t->depth) && (t->depth || !t->depth_whole) &&
		(t->depth_whole || !t->depth_top) && t->band_rows % XSDK_TILE_ROWS == 0;
	const uint32_t rows = valid ? rows_of(t) : 0;
	const uint32_t bands = rows ? (t->height + rows - 1) / rows : 0;
	return bands <= XSDK_TILE_BANDS && rows && band_tiles(t, rows) <= XSDK_EDRAM_TILES ? bands : 0;
}

static uint32_t band_count(void)
{
	return (pass.height + band_rows - 1) / band_rows;
}

static uint32_t rows_in(uint32_t band)
{
	const uint32_t top = band * band_rows;
	return pass.height - top < band_rows ? pass.height - top : band_rows;
}

// The depth from tile 0, the band's or the whole of it, then the colour targets one after another; or with depth_top
// the colour targets from tile 0 and the whole depth after them, ending at eDRAM's last tile.
static void place_targets(void)
{
	const bool first = pass.depth && !pass.depth_top;
	const uint32_t depth_rows = pass.depth_whole ? pass.height : band_rows;
	uint32_t tile = first ? xsdk_gpu_target_sample_tiles(pass.width, depth_rows, pass.samples) : 0;
	for (uint32_t i = 0; i < pass.count; i++) {
		placed[i] = (XsdkColorTarget){ .format = pass.targets[i].format, .tile = tile };
		tile += xsdk_gpu_color_target_sample_tiles(pass.width, band_rows, pass.targets[i].format, pass.samples);
	}
}

// Depth whole moves its base down by the band's rows.
static uint32_t depth_tile_of(uint32_t band)
{
	const uint32_t whole_tiles = xsdk_gpu_target_sample_tiles(pass.width, pass.height, pass.samples);
	const uint32_t whole = pass.depth_top ? XSDK_EDRAM_TILES - whole_tiles : 0;
	return pass.depth_whole ? whole + band * xsdk_gpu_target_sample_tiles(pass.width, band_rows, pass.samples) : 0;
}

NTSTATUS xsdk_gpu_begin_tiles(const XsdkTiling *t, XsdkCommandBuffer *buffer)
{
	assert(!tiling);
	if (!xsdk_gpu_tiling_bands(t)) {
		xsdk_log_write("gpu: a %ux%u pass of %u colour targets%s at %u samples can't be tiled", (unsigned)t->width,
			(unsigned)t->height, (unsigned)t->count, t->depth ? " and depth" : "", (unsigned)t->samples);
		return STATUS_INVALID_PARAMETER;
	}
	pass = *t;
	band_rows = rows_of(t);
	narrowed = false;
	place_targets();
	xsdk_gpu_set_band_targets(
		pass.width, pass.height, band_rows, pass.samples, placed, pass.count, depth_tile_of(0), false);
	xsdk_gpu_record_begin(buffer);
	pass_buffer = buffer;
	tiling = true;
	// Each band's resolve clears its depth with the hierarchical buffer, so the recording can test it, at the sizes
	// xsdk_gpu_hiz_cleared keeps it on.
	if (pass.depth && !pass.depth_whole)
		xsdk_gpu_hiz_cleared(pass.width, band_rows, xsdk_gpu_depth_clear());
	return STATUS_SUCCESS;
}

static void put_bins(GpuPackets *packets, uint32_t opcode, uint64_t bins)
{
	gpu_put_type3(packets, opcode, 2);
	gpu_put(packets, (uint32_t)(bins >> 32));
	gpu_put(packets, (uint32_t)bins);
}

// The draws after carry the predicate bit under a bin mask of the bands the rows cross.
void xsdk_gpu_set_draw_rows(uint32_t top, uint32_t bottom)
{
	assert(tiling && top < bottom && bottom <= pass.height);
	const uint32_t first = top / band_rows;
	const uint32_t last = (bottom - 1) / band_rows;
	const bool every = first == 0 && last == band_count() - 1;
	xsdk_gpu_predicate_draws(!every);
	if (every)
		return;
	GpuPackets packets = gpu_reserve_packets(BIN_DWORDS);
	put_bins(&packets, PM4_SET_BIN_MASK, ((2ull << last) - 1) & ~((1ull << first) - 1));
	gpu_packets_done(&packets);
	narrowed = true;
}

// Whether the last pass's resolves cleared colour target r's to r's clear.
static bool cleared_to(const XsdkTileResolve *r)
{
	for (uint32_t i = 0; i < cleared_count; i++) {
		if (cleared_resolves[i].target == r->target)
			return !memcmp(cleared_resolves[i].clear_rgba, r->clear_rgba, sizeof r->clear_rgba);
	}
	return false;
}

// Whether the last pass's last band left band 0 as these resolves would clear it.
static bool same_layout(const XsdkTileResolve *resolves, uint32_t count)
{
	bool same = band_clear && cleared_rows == band_rows && cleared_depth == xsdk_gpu_depth_clear() &&
		cleared.width == pass.width && cleared.height == pass.height && cleared.samples == pass.samples &&
		cleared.count == pass.count && cleared.depth == pass.depth && cleared.depth_whole == pass.depth_whole &&
		cleared.depth_top == pass.depth_top;
	for (uint32_t i = 0; same && i < pass.count; i++)
		same = cleared.targets[i].format == pass.targets[i].format;
	for (uint32_t i = 0; same && i < count; i++)
		same = resolves[i].drawn_back || resolves[i].target == XSDK_TILE_DEPTH || cleared_to(&resolves[i]);
	return same;
}

// Whether a resolve after the one at `at` copies the same colour target, as a multisampled attachment is copied both
// into its own image and into the one its average goes to.
static bool resolved_again(const XsdkTileResolve *resolves, uint32_t count, uint32_t at, bool before)
{
	for (uint32_t i = at + 1; i < count; i++) {
		if (resolves[i].target == resolves[at].target && (!before || !resolves[i].drawn_back))
			return true;
	}
	return false;
}

// A band's resolves, each clearing its colour unless a later one copies that target too, the last its depth too unless
// it stays whole; band 0's clear, before the first band, leaves out those the recording draws back.
static void resolve_band(const XsdkTileResolve *resolves, uint32_t count, uint32_t band, bool before)
{
	uint32_t last = count;
	for (uint32_t i = 0; i < count; i++) {
		if (!before || !resolves[i].drawn_back)
			last = i;
	}
	for (uint32_t i = 0; i < count; i++) {
		const XsdkTileResolve *const r = &resolves[i];
		assert(r->target < pass.count || (r->target == XSDK_TILE_DEPTH && pass.depth));
		assert(r->texture->width == pass.width && r->texture->height == pass.height);
		if (before && r->drawn_back)
			continue;
		const bool color = !resolved_again(resolves, count, i, before);
		const bool depth = pass.depth && !pass.depth_whole && i == last;
		xsdk_gpu_resolve_band(r->target, r->texture, r->clear_rgba, band * band_rows, rows_in(band), color, depth);
	}
}

// The band's targets, its rows moved up to eDRAM's top with the screen scissor at their size, as Mesa draws a tile
// (mesa:src/gallium/drivers/freedreno/a2xx/fd2_gmem.c:684-688, 711-713), gl_FragCoord's top, and the band's bin.
static void put_band(uint32_t band)
{
	const uint32_t top = band * band_rows;
	// The surface the band's rows tall, so in the last band a whole depth at eDRAM's top ends at its last tile.
	xsdk_gpu_set_band_targets(
		pass.width, pass.height, rows_in(band), pass.samples, placed, pass.count, depth_tile_of(band), true);
	const uint32_t window = ((uint32_t)-(int32_t)top & OFFSET_MASK) << Y_SHIFT;
	const uint32_t screen[] = { 0, SCREEN_WHOLE | rows_in(band) << Y_SHIFT };
	GpuPackets packets = gpu_reserve_packets(WINDOW_DWORDS + (narrowed ? BIN_DWORDS : 0));
	gpu_put_register(&packets, PA_SC_WINDOW_OFFSET, window);
	gpu_put_registers(&packets, PA_SC_SCREEN_SCISSOR_TL, screen, 2);
	if (narrowed)
		put_bins(&packets, PM4_SET_BIN_SELECT, 1ull << band);
	gpu_packets_done(&packets);
	xsdk_gpu_put_frag_coord_top(top);
}

// The screen scissor whole, the bins all ones as XAM leaves them, and gl_FragCoord's constant as outside a pass.
static void put_whole(void)
{
	const uint32_t screen[] = { 0, SCREEN_WHOLE | SCREEN_WHOLE << Y_SHIFT };
	GpuPackets packets = gpu_reserve_packets(3 + (narrowed ? 2 * BIN_DWORDS : 0));
	gpu_put_registers(&packets, PA_SC_SCREEN_SCISSOR_TL, screen, 2);
	if (narrowed) {
		put_bins(&packets, PM4_SET_BIN_MASK, ALL_BINS);
		put_bins(&packets, PM4_SET_BIN_SELECT, ALL_BINS);
	}
	gpu_packets_done(&packets);
	xsdk_gpu_put_frag_coord_top(0);
}

NTSTATUS xsdk_gpu_end_tiles(const XsdkTileResolve *resolves, uint32_t count)
{
	assert(tiling && count >= 1);
	tiling = false;
	xsdk_gpu_predicate_draws(false);
	const NTSTATUS status = xsdk_gpu_record_end();
	if (!NT_SUCCESS(status))
		return status;
	// Band 0's clear: its resolves of what eDRAM holds, which the band then draws over.
	if (!same_layout(resolves, count))
		resolve_band(resolves, count, 0, true);
	for (uint32_t band = 0; band < band_count(); band++) {
		put_band(band);
		xsdk_gpu_execute(pass_buffer);
		resolve_band(resolves, count, band, false);
	}
	put_whole();
	cleared = pass;
	cleared_rows = band_rows;
	cleared_depth = xsdk_gpu_depth_clear();
	cleared_count = count <= RESOLVES_KEPT ? count : 0;
	memcpy(cleared_resolves, resolves, cleared_count * sizeof *resolves);
	band_clear = true;
	return STATUS_SUCCESS;
}
