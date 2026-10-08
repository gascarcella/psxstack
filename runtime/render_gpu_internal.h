/* Between the hardware renderer's files (render_gpu.c, render_gpu_present.c): only in the PSXSTACK_SDL build. */
#ifndef PORT_RENDER_GPU_INTERNAL_H
#define PORT_RENDER_GPU_INTERNAL_H

#ifdef PSXSTACK_SDL
#include <SDL3/SDL.h>

#include "port_harness.h"

/* A shader's embedded blobs for render_shader: SPIR-V everywhere, DXIL too in the Windows build (cmake/psxstack.cmake
 * embeds <name>_spv.h and <name>_dxil.h); RENDER_SHADER_FORMATS is what the device may take (render_gpu_shaders.h). */
#ifdef _WIN32
#define RENDER_SHADER(name) name##_spv, sizeof(name##_spv), name##_dxil, sizeof(name##_dxil)
#define RENDER_SHADER_FORMATS (SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL)
#else
#define RENDER_SHADER(name) name##_spv, sizeof(name##_spv), NULL, 0
#define RENDER_SHADER_FORMATS SDL_GPU_SHADERFORMAT_SPIRV
#endif

/* render_gpu.c: a shader from its SPIR-V or its DXIL (RENDER_SHADER), whichever the device takes, and a pipeline that
 * draws one triangle list into `format` with no blending (the present's state). NULL on failure (SDL_GetError says
 * why). */
SDL_GPUShader *render_shader(const unsigned char *spv, size_t spv_size, const unsigned char *dxil, size_t dxil_size,
                             SDL_GPUShaderStage stage, int samplers, int uniforms);
SDL_GPUGraphicsPipeline *render_pipeline(SDL_GPUShader *vert, SDL_GPUShader *frag, SDL_GPUTextureFormat format);

/* render_gpu_present.c: the present's filter. The fragment uniforms of every filter shader (present_source.hlsli). */
typedef struct {
    Sint32 dst[4];  /* the rectangle in the output: x, y, w, h */
    Sint32 src[4];  /* the picture's size at the internal scale; the VRAM target's size */
    Sint32 cut[4];  /* the picture's corner in the VRAM, the internal scale (0: the image), the display's lines */
    float param[4]; /* the filter's parameters */
} RenderPresentView;

/* The filter's pipelines (for B8G8R8A8, the readback's format, and for `swap_format` unless it is INVALID), created
 * now; PORT_FILTER_NONE releases them. 0 on failure, with the reason in `why` (the caller then presents unfiltered). */
int render_present_set(SDL_GPUDevice *device, SDL_GPUShader *vert, SDL_GPUTextureFormat swap_format,
                       const PortFilter *f, char *why, size_t why_size);
/* The filter's first pass, if it has one (`smooth`: its decisions per source pixel), from `source` (the w x h
 * picture) on cb before the present's render pass; and its texture bound for the present's pass (slot 1). */
void render_present_prepare(SDL_GPUCommandBuffer *cb, SDL_GPUTexture *source, SDL_GPUSampler *sampler, int w, int h);
void render_present_bind(SDL_GPURenderPass *pass, SDL_GPUSampler *sampler);
/* Whether the filter takes the 32-bit image (video.c's 1x software picture) in place of the display cut from the
 * VRAM target: `smooth`, a pixel-art filter, needs the 1x picture at every internal scale. */
int render_present_image(void);
/* The filter's pipeline for the swapchain (swap 1) or the readback target; NULL: none (the present's own). */
SDL_GPUGraphicsPipeline *render_present_pipeline(int swap);
/* The filter's parameters into the view. */
void render_present_params(RenderPresentView *v);
void render_present_release(SDL_GPUDevice *device);
#endif

#endif /* PORT_RENDER_GPU_INTERNAL_H */
