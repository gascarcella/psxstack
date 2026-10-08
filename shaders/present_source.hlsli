// The present filters' common part (runtime/render_gpu_present.c; included by present_<filter>.frag.hlsl): the uniform
// view and the fetch of one source pixel. The source is either video.c's 32-bit image (cut.z 0: a 24-bit display, the
// disabled display, the software image) or a 15-bit display cut from the rasteriser's VRAM target at the internal
// scale cut.z, wrapping at the target's edges, its 8-bit channels v shown as v + (v >> 5) (present_vram.frag.hlsl: at
// scale 1 the software path's colours). The view says nothing about the picture's shape: the source rectangle (its
// size and corner) and the destination rectangle are independent, so a wider cut or another aspect needs no change
// here. SDL_GPU's binding layout: fragment textures t[n] in space2, fragment uniform buffers b[n] in space3.
Texture2D<float4> source : register(t0, space2);
cbuffer Present : register(b0, space3) {
    int4 dst;     // the rectangle in the output: x, y, w, h
    int4 src;     // the picture's size at the internal scale (w, h); the VRAM target's size (w, h)
    int4 cut;     // the picture's corner in the VRAM (x, y, VRAM pixels), the internal scale (0: the image), its lines
    float4 param; // the filter's parameters
};

// Source pixel t (clamped to the picture), 0..255 per channel.
float3 fetch(int2 t) {
    t = clamp(t, int2(0, 0), src.xy - 1);
    if (cut.z == 0) {
        return floor(source.Load(int3(t, 0)).rgb * 255.0 + 0.5);
    }
    int N = cut.z;
    float4 c = source.Load(int3((cut.x * N + t.x) % src.z, (cut.y * N + t.y) % src.w, 0));
    float3 v = floor(c.rgb * 255.0 + 0.5);
    return min(v + floor(v / 32.0), 255.0);
}

// Output pixel p (relative to the rectangle) as the average of the source pixels whose centres fall in it (at most
// 8 x 8): present_vram.frag.hlsl's rule for a rectangle smaller than the picture, which every filter uses then.
float3 box(int2 p) {
    int2 lo = (2 * p * src.xy + dst.zw - 1) / (2 * dst.zw);
    int2 hi = ((2 * p + 2) * src.xy + dst.zw - 1) / (2 * dst.zw);
    hi = max(hi, lo + 1);
    float3 sum = float3(0.0, 0.0, 0.0);
    int n = 0;
    for (int y = lo.y; y < hi.y && y < lo.y + 8; y++) {
        for (int x = lo.x; x < hi.x && x < lo.x + 8; x++) {
            sum += fetch(int2(x, y));
            n++;
        }
    }
    return floor(sum / n + 0.5);
}

// Whether the rectangle is smaller than the picture on an axis (then box() is the picture).
bool shrinking() {
    return dst.z < src.x || dst.w < src.y;
}

int floor_div(int a, int b) {
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

// Sharp bilinear along one axis: source coordinate s (pixel centres at i + 0.5), integer prescale k (the picture
// nearest-scaled k times, then bilinear): the two source pixels and the weight of the second. Continuous in s, so a
// position on a tie gives the same result on every device.
void sharp_axis(float s, int k, out int a, out int b, out float f) {
    float sk = s * k - 0.5;
    int j = int(floor(sk));
    f = sk - j;
    a = floor_div(j, k);
    b = floor_div(j + 1, k);
}

// Sharp bilinear at output position q (continuous, relative to the rectangle; a pixel's centre is p + 0.5).
float3 sharp_at(float2 q) {
    float2 s = q * float2(src.xy) / float2(dst.zw);
    int2 k = max(dst.zw / src.xy, int2(1, 1));
    int ax, bx, ay, by;
    float fx, fy;
    sharp_axis(s.x, k.x, ax, bx, fx);
    sharp_axis(s.y, k.y, ay, by, fy);
    float3 top = lerp(fetch(int2(ax, ay)), fetch(int2(bx, ay)), fx);
    float3 bottom = lerp(fetch(int2(ax, by)), fetch(int2(bx, by)), fx);
    return lerp(top, bottom, fy);
}

// The scanlines' lines: the display's, or half of them for an interlaced picture (more than 288 lines: a TV showed the
// two fields' lines without a gap, so the beams follow one field's).
int beam_lines() {
    return cut.w > 288 ? cut.w / 2 : cut.w;
}

// The scanlines' strength s faded out below two output rows per line (fewer would alias into a moire).
float beam_strength(float s) {
    return s * saturate(float(dst.w) / float(beam_lines()) - 1.0);
}

// Source row y sampled along x at output position qx: sharp bilinear, or the average of the columns whose centres fall
// in the output pixel where the rectangle is narrower than the picture.
float3 row_at(int y, float qx) {
    if (dst.z < src.x) {
        int p = int(floor(qx));
        int lo = (2 * p * src.x + dst.z - 1) / (2 * dst.z);
        int hi = max(((2 * p + 2) * src.x + dst.z - 1) / (2 * dst.z), lo + 1);
        float3 sum = float3(0.0, 0.0, 0.0);
        int n = 0;
        for (int x = lo; x < hi && x < lo + 8; x++) {
            sum += fetch(int2(x, y));
            n++;
        }
        return sum / n;
    }
    int a, b;
    float f;
    sharp_axis(qx * float(src.x) / float(dst.z), max(dst.z / src.x, 1), a, b, f);
    return lerp(fetch(int2(a, y)), fetch(int2(b, y)), f);
}
