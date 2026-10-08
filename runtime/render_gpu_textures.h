/* Texture keys and the texture dump (render_gpu_textures.c; the first game's issue #70, docs/PORT.md "Texture
 * replacement"): what a textured primitive samples, named by what the game loaded rather than where it landed in the
 * VRAM. Only in the PSXSTACK_SDL build. Nothing here runs unless it is switched on (`--dump-textures DIR`): without it
 * gpu.c's listener, the rasteriser and every output are as before. */
#ifndef PORT_RENDER_GPU_TEXTURES_H
#define PORT_RENDER_GPU_TEXTURES_H

#ifdef PSXSTACK_SDL
#include "psyq_internal.h"

/* A textured unit's key: the load whose words it samples (all of them one load's, still as loaded), the CLUT it reads
 * and the depth. */
typedef struct {
    u64 image;      /* the first 8 bytes of the SHA-1 of the load's pixels (w x h 16-bit words, little-endian) */
    u64 clut;       /* the same of the 16 or 256 CLUT entries it reads at the draw; 0 for 15-bit */
    int depth;      /* 4, 8 or 15 (bits per texel) */
    int w, h;       /* the load's size in texels at that depth */
    int u0, v0;     /* the load's top-left texel in the unit's texture page coordinates (u - u0: the load's texel) */
    int vx, vy;     /* the load's place in the VRAM */
} RenderTexKey;

/* --dump-textures DIR: every key the first time a unit samples it, as a PNG under DIR, and DIR/index.json. 0 (logged)
 * when DIR cannot be made. Installs gpu.c's listener when the rasteriser has not (render_gpu_tex_listener). */
int render_gpu_tex_dump_open(const char *dir);
/* Whether keys are tracked (a dump open). */
int render_gpu_tex_active(void);
/* Every event of gpu.c's listener, before the rasteriser records it: the loads' hashes, which load owns each VRAM word,
 * and the dump's textured units. The rasteriser's listener calls it; without the rasteriser it is the listener. */
void render_gpu_tex_event(const GpuEvent *ev);
/* The key of a textured unit sampling texture rows v_lo..v_hi and columns u_lo..u_hi of its page (inclusive, in
 * texels; already through the texture window): 1 with *key filled when every word there belongs to one load, else 0
 * (render-to-texture, a copy, several loads, nothing loaded). */
int render_gpu_tex_key(const GpuEvent *ev, int u_lo, int u_hi, int v_lo, int v_hi, RenderTexKey *key);
/* The end of a vsync (video.c port_video_frame): the index rewritten when keys were added. */
void render_gpu_tex_frame(void);
/* The index written a last time, everything released (video.c port_video_quit). */
void render_gpu_tex_close(void);
#endif

#endif /* PORT_RENDER_GPU_TEXTURES_H */
