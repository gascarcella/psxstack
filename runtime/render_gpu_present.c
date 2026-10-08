/* The hardware renderer's present filters (render_gpu_internal.h; docs/PORT.md "Rendering"): `--filter`, video.filter.
 * A filter is one pixel shader (shaders/present_<filter>.frag.hlsl, on present_source.hlsli) that draws the picture
 * into the window's swapchain, or the readback's target, at video.c's rectangle in one pass, in place of the
 * present's own nearest shaders: no intermediate target, so it costs no memory at any internal scale, and its time
 * depends on the output's size (issue #69: under 0.5 ms at 3840x2160 on an RTX 4070 Ti SUPER, 6 ms at 1080 lines on
 * lavapipe). It reads both sources (the 32-bit image, a 15-bit display cut from the VRAM target), so it applies to the
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
#ifdef _WIN32
#include "present_crt_frag_dxil.h"
#include "present_scanlines_frag_dxil.h"
#include "present_sharp_frag_dxil.h"
#endif

static struct {
    int kind;
    float param[4]; /* the shader's: scanlines, mask, curvature (0..1) */
    SDL_GPUShader *frag;
    SDL_GPUGraphicsPipeline *off, *swap;
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
    default:
        return 1;
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

SDL_GPUGraphicsPipeline *render_present_pipeline(int swap) {
    return swap && f.swap != NULL ? f.swap : f.off;
}

void render_present_params(RenderPresentView *v) {
    memcpy(v->param, f.param, sizeof(v->param));
}
#endif
