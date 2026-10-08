// The present filter `sharp` (runtime/render_gpu_present.c): sharp bilinear. The picture is nearest-scaled by the
// largest integer that fits the rectangle on each axis, then bilinear to the rectangle: every source pixel keeps the
// same size within a pixel, also where the scale is not an integer (a 256- or 368-wide mode at 4:3, internal scale 3
// in a 1080-line window). A rectangle smaller than the picture is the average (present_source.hlsli box()).
#include "present_source.hlsli"

float4 main(float4 pos : SV_Position) : SV_Target0 {
    int2 p = int2(pos.xy) - dst.xy;
    if (shrinking()) {
        return float4(box(p) / 255.0, 1.0);
    }
    return float4(floor(sharp_at(float2(p) + 0.5) + 0.5) / 255.0, 1.0);
}
