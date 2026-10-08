/* The hardware renderer's present filters (render_gpu_internal.h; docs/PORT.md "Rendering"): `--filter`, video.filter,
 * video.crt. A filter is one pixel shader (shaders/present_<filter>.frag.hlsl, on present_source.hlsli) that draws the
 * picture into the window's swapchain, or the readback's target, at video.c's rectangle in one pass, in place of the
 * present's own nearest shaders: no intermediate target at the output's size, so no memory that grows with the
 * internal scale, and its time depends on the output's size (at 3840x2160 on an RTX 4070 Ti SUPER: sharp 0.24 ms, crt
 * 1.0, smooth 0.29). `smooth` alone has a first pass (render_present_prepare): its edge decisions per pixel of the 1x
 * image into a 16-bit texture, which its present reads (render_present_bind), and it always takes the image
 * (render_present_image). It reads both sources (the 32-bit image, a 15-bit display cut from the VRAM target), so it applies to the
 * 24-bit displays too. PORT_FILTER_NONE leaves render_gpu.c's own present untouched: the default picture does not
 * change. Every filter is continuous in its sampling position, so that a pixel centre on a tie (a boundary between
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

static struct {
    int kind;
    float param[4]; /* the shader's: scanlines, mask, curvature (0..1) */
    SDL_GPUShader *frag;
    SDL_GPUGraphicsPipeline *off, *swap;
    /* smooth's first pass: its decisions per source pixel (present_smooth_edges.frag.hlsl) into an R16_UINT texture */
    SDL_GPUShader *edges_frag;
    SDL_GPUGraphicsPipeline *edges_pipe;
    SDL_GPUTexture *edges;
} f;

void render_present_release(SDL_GPUDevice *device) {
    if (f.off != NULL) {
        SDL_ReleaseGPUGraphicsPipeline(device, f.off);
    }
    if (f.swap != NULL) {
        SDL_ReleaseGPUGraphicsPipeline(device, f.swap);
    }
    if (f.frag != NULL) {
        SDL_ReleaseGPUShader(device, f.frag);
    }
    if (f.edges_pipe != NULL) {
        SDL_ReleaseGPUGraphicsPipeline(device, f.edges_pipe);
    }
    if (f.edges_frag != NULL) {
        SDL_ReleaseGPUShader(device, f.edges_frag);
    }
    if (f.edges != NULL) {
        SDL_ReleaseGPUTexture(device, f.edges);
    }
    memset(&f, 0, sizeof(f));
}

int render_present_set(SDL_GPUDevice *device, SDL_GPUShader *vert, SDL_GPUTextureFormat swap_format,
                       const PortFilter *pf, char *why, size_t why_size) {
    render_present_release(device);
    switch (pf->kind) {
    case PORT_FILTER_SHARP:
        f.frag = render_shader(RENDER_SHADER(present_sharp_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
        break;
    case PORT_FILTER_SCANLINES:
        f.frag = render_shader(RENDER_SHADER(present_scanlines_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
        break;
    case PORT_FILTER_CRT:
        f.frag = render_shader(RENDER_SHADER(present_crt_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
        break;
    case PORT_FILTER_SMOOTH:
        /* 2 samplers: the image and the first pass's decisions */
        f.frag = render_shader(RENDER_SHADER(present_smooth_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 1);
        break;
    default:
        return 1;
    }
    if (pf->kind == PORT_FILTER_SMOOTH) {
        SDL_GPUTextureCreateInfo ti;
        memset(&ti, 0, sizeof(ti));
        ti.type = SDL_GPU_TEXTURETYPE_2D;
        ti.format = SDL_GPU_TEXTUREFORMAT_R16_UINT;
        ti.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width = EDGES_W;
        ti.height = EDGES_H;
        ti.layer_count_or_depth = 1;
        ti.num_levels = 1;
        f.edges = SDL_CreateGPUTexture(device, &ti);
        f.edges_frag = render_shader(RENDER_SHADER(present_smooth_edges_frag), SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
        f.edges_pipe =
            f.edges_frag != NULL ? render_pipeline(vert, f.edges_frag, SDL_GPU_TEXTUREFORMAT_R16_UINT) : NULL;
        if (f.edges == NULL || f.edges_pipe == NULL) {
            snprintf(why, why_size, "%s", SDL_GetError());
            render_present_release(device);
            return 0;
        }
    }
    f.off = f.frag != NULL ? render_pipeline(vert, f.frag, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM) : NULL;
    if (f.off != NULL && swap_format != SDL_GPU_TEXTUREFORMAT_INVALID &&
        swap_format != SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM) {
        f.swap = render_pipeline(vert, f.frag, swap_format);
    }
    if (f.off == NULL || (swap_format != SDL_GPU_TEXTUREFORMAT_INVALID &&
                          swap_format != SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM && f.swap == NULL)) {
        snprintf(why, why_size, "%s", SDL_GetError());
        render_present_release(device);
        return 0;
    }
    f.kind = pf->kind;
    f.param[0] = (float)pf->scanlines / 100.0f;
    f.param[1] = (float)pf->mask / 100.0f;
    f.param[2] = (float)pf->curvature / 100.0f;
    return 1;
}

void render_present_prepare(SDL_GPUCommandBuffer *cb, SDL_GPUTexture *source, SDL_GPUSampler *sampler, int w,
                            int h) {
    SDL_GPUColorTargetInfo ct;
    SDL_GPURenderPass *pass;
    SDL_GPUTextureSamplerBinding bind;
    SDL_Rect scissor;
    RenderPresentView v;
    if (f.kind != PORT_FILTER_SMOOTH || w <= 0 || h <= 0 || w > EDGES_W || h > EDGES_H) {
        return;
    }
    memset(&ct, 0, sizeof(ct));
    ct.texture = f.edges;
    ct.load_op = SDL_GPU_LOADOP_DONT_CARE;
    ct.store_op = SDL_GPU_STOREOP_STORE;
    pass = SDL_BeginGPURenderPass(cb, &ct, 1, NULL);
    SDL_BindGPUGraphicsPipeline(pass, f.edges_pipe);
    scissor.x = 0;
    scissor.y = 0;
    scissor.w = w;
    scissor.h = h;
    SDL_SetGPUScissor(pass, &scissor);
    bind.texture = source;
    bind.sampler = sampler;
    SDL_BindGPUFragmentSamplers(pass, 0, &bind, 1);
    memset(&v, 0, sizeof(v));
    v.dst[2] = w;
    v.dst[3] = h;
    v.src[0] = w;
    v.src[1] = h; /* cut.z 0: the image */
    SDL_PushGPUFragmentUniformData(cb, 0, &v, sizeof(v));
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
}

void render_present_bind(SDL_GPURenderPass *pass, SDL_GPUSampler *sampler) {
    SDL_GPUTextureSamplerBinding bind;
    if (f.kind == PORT_FILTER_SMOOTH) {
        bind.texture = f.edges;
        bind.sampler = sampler;
        SDL_BindGPUFragmentSamplers(pass, 1, &bind, 1);
    }
}

int render_present_image(void) {
    return f.kind == PORT_FILTER_SMOOTH;
}

SDL_GPUGraphicsPipeline *render_present_pipeline(int swap) {
    return swap && f.swap != NULL ? f.swap : f.off;
}

void render_present_params(RenderPresentView *v) {
    memcpy(v->param, f.param, sizeof(v->param));
}
#endif
