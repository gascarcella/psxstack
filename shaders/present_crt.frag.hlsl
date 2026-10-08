// The present filter `crt` (runtime/render_gpu_present.c): a CRT, in linear light (2.2). Each of the display's lines
// (beam_lines()) is a beam across the rectangle with a Gaussian profile that widens with brightness, its area
// normalised so that the picture keeps its brightness; at strength 0 the beams are tents, a plain vertical blend. The
// four lines around a position are summed, so the result is continuous where the nearest line changes (a pixel centre
// on a boundary is a tie, rounded differently by different devices). An aperture grille (output columns in red, green,
// blue) dims the other two channels, compensated in the average; an optional barrel curvature bends the picture into
// the rectangle (black outside, its edge faded over one output pixel). param: x scanlines, y mask, z curvature (video.crt's / 100). Horizontally each line
// is sampled at the source's resolution (row_at: sharp bilinear, or averaged in a narrower rectangle).
#include "present_source.hlsli"

// Line l's colour in linear light: the mean of two of its source rows (of an interlaced line's two fields' rows), at
// output position qx.
float3 line_at(int l, float qx) {
    int per = max(src.y / beam_lines(), 1); // source rows per line
    l = clamp(l, 0, beam_lines() - 1);
    float3 c = (row_at(l * per + per / 4, qx) + row_at(l * per + (3 * per) / 4, qx)) * 0.5;
    return pow(c / 255.0, 2.2);
}

// Line colour c's weight at distance d (in lines) from its centre.
float3 beam(float d, float3 c, float s) {
    float3 sigma = lerp(0.22, 0.45, sqrt(c));
    float3 g = exp(-(d * d) / (2.0 * sigma * sigma)) / (sigma * 2.50662827463);
    float tent = saturate(1.0 - abs(d));
    return lerp(float3(tent, tent, tent), g, s) * c;
}

float4 main(float4 pos : SV_Position) : SV_Target0 {
    float2 q = pos.xy - float2(dst.xy); // the output position in the rectangle
    float edge = 1.0;
    if (param.z > 0.0) {
        float2 c = q / float2(dst.zw) * 2.0 - 1.0;
        c *= 1.0 + param.z * 0.12 * (c.yx * c.yx);
        q = (c * 0.5 + 0.5) * float2(dst.zw);
        // the bent edge fades to black over one output pixel: continuous (no tie at the edge), and smooth
        float2 e = saturate(min(q, float2(dst.zw) - q));
        edge = e.x * e.y;
        if (edge <= 0.0) {
            return float4(0.0, 0.0, 0.0, 1.0);
        }
    }
    float s = beam_strength(param.x);
    float ly = q.y * float(beam_lines()) / float(dst.w) - 0.5;
    int l0 = int(floor(ly));
    float d = ly - l0;
    float3 col = float3(0.0, 0.0, 0.0);
    for (int i = -1; i <= 2; i++) {
        col += beam(d - i, line_at(l0 + i, q.x), s);
    }
    float m = param.y;
    int x = int(pos.x) % 3;
    float3 grille = float3(x == 0 ? 1.0 : 1.0 - 0.75 * m, x == 1 ? 1.0 : 1.0 - 0.75 * m, x == 2 ? 1.0 : 1.0 - 0.75 * m);
    col *= grille / (1.0 - 0.5 * m) * edge;
    return float4(floor(pow(saturate(col), 1.0 / 2.2) * 255.0 + 0.5) / 255.0, 1.0);
}
