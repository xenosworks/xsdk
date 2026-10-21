#ifndef XSDK_TEXT_H
#define XSDK_TEXT_H

#include <stdint.h>

#include "xsdk/kernel.h"

#ifdef __cplusplus
extern "C" {
#endif

// Text in X11's misc-fixed 10x20, a public-domain bitmap font of Latin, Greek, Cyrillic, box drawing and symbols,
// drawn through the GPU layer by the drawing thread (docs/GUIDE.md, "Text on screen").

enum {
	// The font's cell, in pixels.
	XSDK_TEXT_CELL_WIDTH = 10,
	XSDK_TEXT_CELL_HEIGHT = 20,
};

// The font's atlas, the text shaders and a vertex ring in GPU memory; the GPU open.
NTSTATUS xsdk_text_open(void);
// Frees them once the GPU is done with them; before xsdk_gpu_close.
void xsdk_text_close(void);
// Queues utf8 with its first cell's top-left at (x, y) pixels from the viewport's, each cell scale times the font's,
// 1 to 8, in colour argb (0xAARRGGBB), blended over what's drawn; '\n' starts the next line at x.
void xsdk_text_draw(int32_t x, int32_t y, uint32_t scale, uint32_t argb, const char *utf8);
// The pixels utf8 takes at scale: its longest line's width, and its lines' height.
void xsdk_text_measure(const char *utf8, uint32_t scale, uint32_t *width, uint32_t *height);

#ifdef __cplusplus
}
#endif

#endif
