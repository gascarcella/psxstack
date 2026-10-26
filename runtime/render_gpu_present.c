/* The hardware renderer's present filters (render_gpu_internal.h; docs/PORT.md "Rendering"): `--filter`, video.filter,
 * video.crt. A filter is one pixel shader (shaders/present_<filter>.frag.hlsl, on present_source.hlsli) that draws the
 * picture into the window's swapchain, or the readback's target, at video.c's rectangle in one pass, in place of the
 * present's own nearest shaders: no intermediate target at the output's size, so no memory that grows with the
 * internal scale, and its time depends on the output's size (at 3840x2160 on an RTX 4070 Ti SUPER: sharp 0.24 ms, crt
 * 1.0, smooth 0.29). `smooth` alone has a first pass (render_present_prepare): its edge decisions per pixel of the 1x
 * image into a 16-bit texture, which its present reads (render_present_bind), and it always takes the image
 * (render_present_image). It reads both sources (the 32-bit image, a 15-bit display cut from the VRAM target), so it applies to the
 * 24-bit displays too. PORT_FILTER_NONE leaves render_gpu.c's own present untouched: the default picture does not
 * change. Two slots, the scenes' two sets (GAME_CONTRACT.md "1. game.json" video.scenes), both kept built: a scene
 * change picks the slot render_gpu.c passes, and builds nothing. Every filter is continuous in its sampling position, so that a pixel centre on a tie (a boundary between
 * source pixels or lines) comes out the same on every device. */
#ifdef PSXSTACK_SDL
#include <stdio.h>
#include <string.h>

#include "render_gpu_internal.h"

#include "present_crt_frag_spv.h"
#include "present_scanlines_frag_spv.h"
#include "present_sharp_frag_spv.h"
#include "present_smooth_edges_frag_spv.h"
#include "present_smooth_frag_spv.h"
#ifdef _WIN32
#include "present_crt_frag_dxil.h"
#include "present_scanlines_frag_dxil.h"
#include "present_sharp_frag_dxil.h"
#include "present_smooth_edges_frag_dxil.h"
#include "present_smooth_frag_dxil.h"
#endif

#define EDGES_W 640 /* smooth's decisions: the largest 32-bit image (render_gpu.c RENDER_IMAGE_W, RENDER_IMAGE_H) */
#define EDGES_H 576

/* Two slots (the scenes' sets: PORT_SCENE_3D, PORT_SCENE_2D; a game without scenes uses slot 0), both built by
 * render_present_set: a scene change only picks the slot the present binds, nothing is created or freed. A slot is a
 * kind and its parameters; the pipelines are per kind, so two slots of the same kind share them. */
static struct {
    int kind;
    float param[4]; /* the shader's: scanlines, mask, curvature (0..1) */
} f[2];
static struct {
    SDL_GPUShader *frag;
    SDL_GPUGraphicsPipeline *off, *swap;
} kinds[PORT_FILTER_COUNT];
/* smooth's first pass: its decisions per source pixel (present_smooth_edges.frag.hlsl) into an R16_UINT texture, one
 * for both slots (one present at a time) */
static SDL_GPUShader *edges_frag;
static SDL_GPUGraphicsPipeline *edges_pipe;
static SDL_GPUTexture *edges;

static void present_release_kind(SDL_GPUDevice *device, int kind) {
    if (kinds[kind].off != NULL) {
        SDL_ReleaseGPUGraphicsPipeline(device, kinds[kind].off);
    }
    if (kinds[kind].swap != NULL) {
        SDL_ReleaseGPUGraphicsPipeline(device, kinds[kind].swap);
    }
    if (kinds[kind].frag != NULL) {
        SDL_ReleaseGPUShader(device, kinds[kind].frag);
    }
    memset(&kinds[kind], 0, sizeof(kinds[kind]));
    if (kind == PORT_FILTER_SMOOTH) {
        if (edges_pipe != NULL) {
            SDL_ReleaseGPUGraphicsPipeline(device, edges_pipe);
        }
        if (edges_frag != NULL) {
            SDL_ReleaseGPUShader(device, edges_frag);
        }
        if (edges != NULL) {
            SDL_ReleaseGPUTexture(device, edges);
        }
        edges_pipe = NULL;
        edges_frag = NULL;
        edges = NULL;
    }
}

void render_present_release(SDL_GPUDevice *device) {
    int k;
    for (k = 0; k < PORT_FILTER_COUNT; k++) {
        present_release_kind(device, k);
    }
    memset(f, 0, sizeof(f));
}

/* The pipelines of `kind` (built once: a second slot of the kind shares them). 0 on failure, with the reason in why. */
static int present_build(SDL_GPUDevice *device, SDL_GPUShader *vert, SDL_GPUTextureFormat swap_format, int kind,
                         char *why, size_t why_size) {
    if (kinds[kind].off != NULL) {
        return 1;
    }
    switch (kind) {
    case PORT_FILTER_SHARP:
        kinds[kind].frag = render_shader(RENDER_SHADER(present_sharp_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
        break;
    case PORT_FILTER_SCANLINES:
        kinds[kind].frag = render_shader(RENDER_SHADER(present_scanlines_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
        break;
    case PORT_FILTER_CRT:
        kinds[kind].frag = render_shader(RENDER_SHADER(present_crt_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
        break;
    case PORT_FILTER_SMOOTH:
        /* 2 samplers: the image and the first pass's decisions */
        kinds[kind].frag = render_shader(RENDER_SHADER(present_smooth_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
        break;
    default:
        return 1;
    }
    if (kind == PORT_FILTER_SMOOTH) {
        SDL_GPUTextureCreateInfo ti;
        memset(&ti, 0, sizeof(ti));
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_R16_UINT;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = EDGES_W;
        ti.height = EDGES_H;
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        edges = SDL_CreateGPUTexture(device, &ti);
        edges_frag = render_shader(RENDER_SHADER(present_smooth_edges_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
        edges_pipe = edges_frag != NULL ? render_pipeline(vert, edges_frag, SDL_GPU_TEXTUREFORMAT_R16_UINT) : NULL;
        if (edges == NULL || edges_pipe == NULL) {
            snprintf(why, why_size, "%s", SDL_GetError());
            present_release_kind(device, kind);
            return 0;
        }
    }
    kinds[kind].off =
        kinds[kind].frag != NULL ? render_pipeline(vert, kinds[kind].frag, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM) : NULL;
    if (kinds[kind].off != NULL && swap_format != SDL_GPU_TEXTUREFORMAT_INVALID &&
        swap_format != SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM) {
        kinds[kind].swap = render_pipeline(vert, kinds[kind].frag, swap_format);
    }
    if (kinds[kind].off == NULL || (swap_format != SDL_GPU_TEXTUREFORMAT_INVALID &&
                                    swap_format != SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM && kinds[kind].swap == NULL)) {
        snprintf(why, why_size, "%s", SDL_GetError());
        present_release_kind(device, kind);
        return 0;
    }
    return 1;
}

void render_present_set(SDL_GPUDevice *device, SDL_GPUShader *vert, SDL_GPUTextureFormat swap_format,
                        const PortFilter pf[2], int ok[2], char *why, size_t why_size) {
    int i;
    render_present_release(device);
    for (i = 0; i < 2; i++) {
        ok[i] = present_build(device, vert, swap_format, pf[i].kind, why, why_size);
        if (!ok[i]) {
            continue; /* this slot presents unfiltered */
        }
        f[i].kind = pf[i].kind;
        f[i].param[0] = (float)pf[i].scanlines / 100.0f;
        f[i].param[1] = (float)pf[i].mask / 100.0f;
        f[i].param[2] = (float)pf[i].curvature / 100.0f;
    }
}

void render_present_prepare(int slot, SDL_GPUCommandBuffer *cb, SDL_GPUTexture *source, SDL_GPUSampler *sampler,
                            const RenderPresentView *view) {
    int w = view->src[0], h = view->src[1];
    SDL_GPUColorTargetInfo ct;
    SDL_GPURenderPass *pass;
    SDL_GPUTextureSamplerBinding bind;
    SDL_Rect scissor;
    RenderPresentView v;
    if (f[slot].kind != PORT_FILTER_SMOOTH || w <= 0 || h <= 0 || w > EDGES_W || h > EDGES_H) {
        return;
    }
    memset(&ct, 0, sizeof(ct));
    ct.texture = edges;
    ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    pass = SDL_BeginGPURenderPass(cb, &ct, 1, NULL);
    SDL_BindGPUGraphicsPipeline(pass, edges_pipe);
    scissor.x = 0;
    scissor.y = 0;
    scissor.w = w;
    scissor.h = h;
    SDL_SetGPUScissor(pass, &scissor);
    bind.texture = source;
    bind.sampler = sampler;
    SDL_BindGPUFragmentSamplers(pass, 0, &bind, 1);
    v = *view; /* the picture as the present reads it, into a w x h target */
    v.dst[0] = 0;
    v.dst[1] = 0;
    v.dst[2] = w;
    v.dst[3] = h;
    SDL_PushGPUFragmentUniformData(cb, 0, &v, sizeof(v));
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

void render_present_bind(int slot, SDL_GPURenderPass *pass, SDL_GPUSampler *sampler) {
    SDL_GPUTextureSamplerBinding bind;
    if (f[slot].kind == PORT_FILTER_SMOOTH) {
        bind.texture = edges;
        bind.sampler = sampler;
        SDL_BindGPUFragmentSamplers(pass, 1, &bind, 1);
    }
}

int render_present_image(int slot) {
    return f[slot].kind == PORT_FILTER_SMOOTH;
}

SDL_GPUGraphicsPipeline *render_present_pipeline(int slot, int swap) {
    int k = f[slot].kind;
    return swap && kinds[k].swap != NULL ? kinds[k].swap : kinds[k].off;
}

void render_present_params(int slot, RenderPresentView *v) {
    memcpy(v->param, f[slot].param, sizeof(v->param));
}
#endif
