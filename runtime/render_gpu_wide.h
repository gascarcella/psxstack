/* The hardware renderer's wide canvas (render_gpu_wide.c; docs/PORT.md "Rendering"): render_gpu.c's side. The canvas
 * is a strip of the rasteriser's target right of its 1024-pixel VRAM, RENDER_WIDE_COLS VRAM pixels wide, allocated when
 * widescreen is enabled before the rasteriser starts. */
#ifndef PORT_RENDER_GPU_WIDE_H
#define PORT_RENDER_GPU_WIDE_H

#ifdef PSXSTACK_SDL
#include "psyq_internal.h"

#define RENDER_WIDE_COLS 512

/* render_gpu_wide.c, for render_gpu.c: whether the target gets the strip (render_gpu_wide_enable was called); an event
 * before render_gpu.c records it (a display buffer gets its canvas) and after (its copy in the canvas); the canvas
 * corner (cut[0], cut[1], target VRAM pixels) of the display at vram_xy when it is presented wide (w: the canvas's
 * width). */
int render_gpu_wide_enabled(void);
void render_gpu_wide_before(const GpuEvent *ev);
void render_gpu_wide_event(const GpuEvent *ev);
int render_gpu_wide_cut(const int vram_xy[2], int w, int h, int cut[2]);

/* render_gpu.c, for render_gpu_wide.c: units recorded into the target at this point of the frame. A triangle or
 * rectangle event already moved into the canvas (pair_x0: the drawing area's x0 for gpu.c's per-pair colour rule,
 * kept from the original so a span's start inside the picture does not move); an opaque fill; a copy of the target's
 * pixels (the canvas gets what the VRAM part drew). */
void render_gpu_unit_event(const GpuEvent *ev, int pair_x0);
void render_gpu_unit_fill(int x, int y, int w, int h, u16 color);
void render_gpu_unit_mirror(int sx, int sy, int dx, int dy, int w, int h);
#endif

#endif /* PORT_RENDER_GPU_WIDE_H */
