/* The hardware renderer's compiled shaders (render_gpu.c), embedded by cmake/psxstack.cmake under gen/shaders:
 * SPIR-V for SDL_GPU's Vulkan backend everywhere, and in the Windows build DXIL for its D3D12 backend too (the HLSL
 * follows both backends' binding layouts: SDL builds the D3D12 root signature from the counts render_shader passes,
 * so the shaders that only Load declare no sampler). RENDER_SHADER(name) is a shader's two blobs for render_shader
 * (no DXIL outside Windows); RENDER_SHADER_FORMATS is what the device may take. With both, SDL tries D3D12 before
 * Vulkan; SDL's SDL_GPU_DRIVER (vulkan, direct3d12) picks one. */
#ifndef PSXSTACK_RENDER_GPU_SHADERS_H
#define PSXSTACK_RENDER_GPU_SHADERS_H

#include "present_frag_spv.h"
#include "present_vert_spv.h"
#include "present_vram_frag_spv.h"
#include "raster_frag_spv.h"
#include "raster_vert_spv.h"

#ifdef _WIN32
#include "present_frag_dxil.h"
#include "present_vert_dxil.h"
#include "present_vram_frag_dxil.h"
#include "raster_frag_dxil.h"
#include "raster_vert_dxil.h"
#endif
/* RENDER_SHADER and RENDER_SHADER_FORMATS: render_gpu_internal.h (the present filters' shaders, render_gpu_present.c,
 * include their own blobs: each blob is a static array, unused in the other file). */

#endif
