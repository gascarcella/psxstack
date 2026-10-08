/* Texture keys, the texture dump and texture packs (render_gpu_textures.c, render_gpu_packs.c; the first game's issue
 * #70, docs/PORT.md "Texture replacement"): what a textured primitive samples, named by what the game loaded rather
 * than where it landed in the VRAM, and the packs' replacements for it. Only in the PSXSTACK_SDL build. Nothing here
 * runs unless it is switched on (`--dump-textures DIR`, a pack): without it gpu.c's listener, the rasteriser and every
 * output are as before. */
#ifndef PORT_RENDER_GPU_TEXTURES_H
#define PORT_RENDER_GPU_TEXTURES_H

#ifdef PSXSTACK_SDL
#include <SDL3/SDL.h>

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
 * when DIR cannot be made. Starts tracking keys (render_gpu_tex_track). */
int render_gpu_tex_dump_open(const char *dir);
/* Whether keys are tracked (a dump or a pack open). render_gpu_tex_track starts tracking them: gpu.c's listener is
 * render_gpu_tex_event until the rasteriser installs its own, which forwards to it. */
int render_gpu_tex_active(void);
void render_gpu_tex_track(void);
/* Every event of gpu.c's listener, after the rasteriser recorded it (its replacement looked up first): the loads'
 * hashes, which load owns each VRAM word, and the dump's textured units. */
void render_gpu_tex_event(const GpuEvent *ev);
/* The key of a textured unit sampling texture rows v_lo..v_hi and columns u_lo..u_hi of its page (inclusive, in
 * texels; already through the texture window): 1 with *key filled when every word there belongs to one load, else 0
 * (render-to-texture, a copy, several loads, nothing loaded). */
int render_gpu_tex_key(const GpuEvent *ev, int u_lo, int u_hi, int v_lo, int v_hi, RenderTexKey *key);
/* The key of a textured triangle or rectangle event and its texel range (u_lo, u_hi, v_lo, v_hi: gpu.c's, a
 * triangle's far edges not sampled, through the texture window): render_gpu_tex_key on that range. */
int render_gpu_tex_unit_key(const GpuEvent *ev, RenderTexKey *key, int range[4]);
/* The end of a vsync (video.c port_video_frame): the index rewritten when keys were added. */
void render_gpu_tex_frame(void);
/* The index written a last time, everything released (video.c port_video_quit). */
void render_gpu_tex_close(void);

/* ---- Texture packs (render_gpu_packs.c; docs/RUNTIME.md "Texture packs") ---- */

/* A texture pack: DIR/mod.json (a data mod with "textures") and the PNGs under its textures directory, named by key.
 * Packs added first win. 0 (logged) when DIR is not one. Starts tracking keys. */
int render_gpu_tex_pack_add(const char *dir);
int render_gpu_tex_packs(void); /* how many packs are loaded */
/* A replacement: its texture (made and uploaded on first use) and the rectangle of the unit's texture page it covers
 * (u0, v0, w, h in texels), sampled linearly or nearest. */
typedef struct {
    SDL_GPUTexture *texture;
    int linear;
    int u0, v0, w, h;
} RenderTexReplacement;
/* The replacement for a textured triangle or rectangle event, called before the event's own writes change the owner
 * map: the first pack's file for its key whose rectangle holds the unit's texels (a sub-rectangle file before the
 * whole image's). 1 with *out, else 0. */
int render_gpu_tex_replacement(SDL_GPUDevice *device, const GpuEvent *ev, RenderTexReplacement *out);
/* After a frame's units were submitted: textures unused for longest released while over the budget. */
void render_gpu_tex_packs_frame(SDL_GPUDevice *device);
/* Every texture released (before the device is destroyed); the packs stay loaded. */
void render_gpu_tex_packs_release(SDL_GPUDevice *device);
#endif

#endif /* PORT_RENDER_GPU_TEXTURES_H */
