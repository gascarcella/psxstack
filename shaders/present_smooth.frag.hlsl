// The present filter `smooth` (runtime/render_gpu_present.c): a 2D smoothing filter for pixel art, always on the 1x
// software image (render_gpu.c presents the image, not the target's cut, for it: the internal scale's blocks of 1x
// texels would defeat it). Our own implementation of xBR level 2 (Hyllian's published algorithm): the first pass
// (present_smooth_edges.frag.hlsl) decided, for each source pixel's four corners, whether an edge crosses it (45
// degrees, or shallow or steep) and which neighbour's colour lies beyond; here each corner is blended towards that
// colour behind the edge's line. The picture is evaluated at the centres of cells k times smaller than a source pixel
// (k: the largest integer scale that fits, as `sharp`), each line's blend ramping over one cell, and the cells are
// blended bilinearly into the rectangle: continuous in the output position, as every filter. A rectangle smaller than
// the picture is the average (box()).
#include "present_source.hlsli"

Texture2D<uint> edges : register(t1, space2);

// The blend towards a line: x is the distance past the line in source pixels, w the cell's size: 0 before, 1 past.
float ramp(float x, float w) {
    return saturate(x / w + 0.5);
}

// The cell centred at sub-position uv (0..1) of source pixel P, w the cell's size.
float3 cell(int2 P, float2 uv, float w) {
    float3 res = fetch(P);
    uint bits = edges.Load(int3(clamp(P, int2(0, 0), src.xy - 1), 0));
    for (int corner = 0; corner < 4; corner++) {
        uint b = bits >> (4 * corner);
        if ((b & 1u) != 0) {
            int sx = (corner & 1) ? -1 : 1, sy = (corner & 2) ? -1 : 1;
            float3 beyond = fetch(P + ((b & 2u) != 0 ? int2(0, sy) : int2(sx, 0)));
            float u = sx > 0 ? uv.x : 1.0 - uv.x, v = sy > 0 ? uv.y : 1.0 - uv.y;
            float a = ramp((u + v - 1.5) * 0.70710678, w); // 45 degrees, through (1, 0.5) and (0.5, 1)
            if ((b & 4u) != 0) {
                a = max(a, ramp((0.5 * u + v - 1.0) * 0.89442719, w)); // shallow, through (1, 0.5) and (0, 1)
            }
            if ((b & 8u) != 0) {
                a = max(a, ramp((u + 0.5 * v - 1.0) * 0.89442719, w)); // steep, through (0.5, 1) and (1, 0)
            }
            res = lerp(res, beyond, a);
        }
    }
    return res;
}

// Cell q of the picture at k times its size.
float3 cell_at(int2 q, int2 k) {
    int2 P = int2(floor_div(q.x, k.x), floor_div(q.y, k.y));
    float2 uv = (float2(q - P * k) + 0.5) / float2(k);
    return cell(P, uv, 1.0 / float(max(k.x, k.y)));
}

float4 main(float4 pos : SV_Position) : SV_Target0 {
    int2 p = int2(pos.xy) - dst.xy;
    if (shrinking()) {
        return float4(box(p) / 255.0, 1.0);
    }
    // bilinear between the cells (sharp bilinear, with the cells as the picture's pixels)
    int2 k = max(dst.zw / src.xy, int2(1, 1));
    float2 s = (float2(p) + 0.5) * float2(src.xy * k) / float2(dst.zw) - 0.5;
    int2 a = int2(floor(s));
    float2 f = s - float2(a);
    float3 top = lerp(cell_at(a, k), cell_at(a + int2(1, 0), k), f.x);
    float3 bottom = lerp(cell_at(a + int2(0, 1), k), cell_at(a + int2(1, 1), k), f.x);
    return float4(floor(lerp(top, bottom, f.y) + 0.5) / 255.0, 1.0);
}
