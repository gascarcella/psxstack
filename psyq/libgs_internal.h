/* psyq/libgs_internal.h: LIBGS's state, shared by libgs.c (the matrices, the coordinate systems, the set-up) and
 * libgs_sort.c (the double buffer, GsSortObject4 and its handlers). libgs.c owns it (its reset and save state) and
 * calls nothing of libgs_sort.c's, so that a harness can build libgs.c with LIBGTE and the GTE alone (the first game's
 * tests/host/libgs_replay.py does). */
#ifndef PSYQ_LIBGS_INTERNAL_H
#define PSYQ_LIBGS_INTERNAL_H

#include "psxstack/psyq/libgpu.h"
#include "psxstack/psyq/libgs.h"

typedef struct {
    s32 projection;               /* GsSetProjection: the distance to the screen (h) */
    s32 light_mode;               /* GsSetLightMode (0..3; GsSortObject4 uses bit 0, fog) */
    u16 w, h, intl, dither, vram; /* GsInitGraph */
    GsCOORDINATE2 *lw_stack[100]; /* GsGetLw's walk up the hierarchy */
    /* The double buffer (GsInitGraph's record, GsInit3D, GsSwapDispBuff) */
    DISPENV disp;     /* LIBGS's display environment: the screen, at the buffer shown */
    DRAWENV draw;     /* LIBGS's drawing environment: its clip is the buffer drawn */
    RECT clip;        /* the clip rectangle in a buffer: 0, 0, w, h */
    s16 draw_ofs[2];  /* the screen's origin in a buffer (GsInit3D: its centre) */
    s16 buf_x[2];     /* the two buffers' VRAM origins (GsDefDispBuff's; not in the shim, so 0, 0 both) */
    s16 buf_y[2];
    s16 idx;          /* PSDIDX: the buffer drawn */
    s16 ofsgpu;       /* GsInitGraph's intl & 4 (GsOFSGPU): the offset goes to the GPU, not the GTE */
    s16 gte_ofs[2];   /* the offset last given to the GTE (0, 0 with GsOFSGPU) */
    s32 zmin, zmax;   /* GsInit3D: LIBGS's Z range */
    /* GsSortObject4's decode of the object's attribute (the handlers read abe) */
    u32 material, lmode, llmod, loff, div, abe;
} PsyqGs;

extern PsyqGs psyq_gs;
extern u32 D_800812D8; /* PSDCNT: the frame count GsGetLw's cache compares with (GsSwapDispBuff counts it) */

#endif /* PSYQ_LIBGS_INTERNAL_H */
