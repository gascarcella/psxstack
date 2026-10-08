// The present filter `scanlines` (runtime/render_gpu_present.c): sharp bilinear (present_sharp.frag.hlsl), each of the
// display's lines darkened towards its edges by a raised cosine: 1 - s (1 + cos 2 pi t) / 2 at position t in the line
// (beam_lines(): half of an interlaced picture's), times 1 + s / 2 so that the average stays near the picture's; periodic,
// so continuous across lines. param.x: the strength s (video.crt.scanlines / 100). Where the rectangle is smaller than
// the picture the colour is the average (box()); the lines stay while there are two output rows per line.
#include "present_source.hlsli"

float4 main(float4 pos : SV_Position) : SV_Target0 {
    int2 p = int2(pos.xy) - dst.xy;
    float3 c = shrinking() ? box(p) : sharp_at(float2(p) + 0.5);
    float s = beam_strength(param.x);
    float ly = (float(p.y) + 0.5) * float(beam_lines()) / float(dst.w);
    float t = ly - floor(ly);
    float w = (1.0 - s * (0.5 + 0.5 * cos(6.28318530718 * t))) * (1.0 + 0.5 * s);
    return float4(min(floor(c * w + 0.5), 255.0) / 255.0, 1.0);
}
