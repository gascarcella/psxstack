/* runtime/render_gpu_subpixel.h: render_gpu.c's side of sub-pixel precision (render_gpu_subpixel.c; docs/PORT.md
 * "Sub-pixel precision"). */
#ifndef RENDER_GPU_SUBPIXEL_H
#define RENDER_GPU_SUBPIXEL_H

#include "psyq_internal.h"

/* A precise triangle's attribute planes, raster.frag.hlsl's `sp`: [0] the origin (vertex a's position, VRAM pixels),
 * [1] q, [2..6] u, v, r, g, b times q; each plane is (value at the origin, d/dx, d/dy, 0) and an attribute at a sample
 * point is plane / q (q = 1: affine). */
#define SUBPIXEL_PLANES 7

/* The rasteriser started at `scale`: the GTE shadow on when the setting is and the scale is above 1. */
void render_subpixel_start(int scale);
/* The rasteriser stops: the shadow off, its counts logged. */
void render_subpixel_stop(void);
/* A triangle's vertices at their sub-pixel positions (VRAM pixels) and its planes: 1 when at least one of its vertices
 * has one (the triangle is then drawn with them), 0 otherwise (pos and planes untouched). */
int render_subpixel_triangle(const GpuEvent *ev, float pos[3][2], float planes[SUBPIXEL_PLANES][4]);

#endif
