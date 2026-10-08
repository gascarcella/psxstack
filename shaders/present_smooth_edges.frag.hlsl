// The present filter `smooth`'s first pass (runtime/render_gpu_present.c; present_smooth.frag.hlsl draws with it): for
// every pixel P of the 1x image, xBR level 2's decisions for its four corners (our own implementation of Hyllian's
// published algorithm), into a 16-bit integer target of the image's size, 4 bits per corner c (0: bottom-right,
// 1: bottom-left, 2: top-right, 3: top-left): bit 4c an edge crosses the corner, 4c + 1 the colour beyond it is the
// vertical neighbour's (else the horizontal one's), 4c + 2 the edge is shallow, 4c + 3 steep. Integers only (a
// weighted YUV distance), so the decisions are the same on every device. Done once per present at the image's size,
// so the second pass, per output pixel, reads one word and a few texels instead of a 5x5 neighbourhood per cell.
#include "present_source.hlsli"

int3 yuv(float3 c) {
    int3 i = int3(c);
    return int3(299 * i.r + 587 * i.g + 114 * i.b, -169 * i.r - 331 * i.g + 500 * i.b, 500 * i.r - 419 * i.g - 81 * i.b);
}

// The distance of two colours (yuv()), 0..15555000; equal below 15 units of 0..255 colour.
int dist(int3 a, int3 b) {
    int3 d = abs(a - b);
    return 48 * d.x + 7 * d.y + 6 * d.z;
}

bool eq(int3 a, int3 b) {
    return dist(a, b) < 15000;
}

uint main(float4 pos : SV_Position) : SV_Target0 {
    int2 P = int2(pos.xy);
    int3 y[5][5]; // the 5x5 neighbourhood, y[row][col] for (col - 2, row - 2)
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 5; col++) {
            y[row][col] = yuv(fetch(P + int2(col - 2, row - 2)));
        }
    }
    uint bits = 0;
    for (int corner = 0; corner < 4; corner++) {
        int sx = (corner & 1) ? -1 : 1, sy = (corner & 2) ? -1 : 1; // right/left, bottom/top
        // in the corner's frame (bottom-right): x' = sx x, y' = sy y
        #define N(X, Y) y[2 + sy * (Y)][2 + sx * (X)]
        int3 E = N(0, 0), F = N(1, 0), H = N(0, 1), I = N(1, 1), B = N(0, -1), D = N(-1, 0), C = N(1, -1),
             G = N(-1, 1), F4 = N(2, 0), I4 = N(2, 1), H5 = N(0, 2), I5 = N(1, 2);
        #undef N
        // the differences along the / diagonal (through H and F) and along the \ one (through E and I): an edge runs
        // between E and I where the / direction is the smoother
        int slash = dist(E, C) + dist(E, G) + dist(I, F4) + dist(I, H5) + 4 * dist(H, F);
        int backslash = dist(H, D) + dist(H, I5) + dist(F, I4) + dist(F, B) + 4 * dist(E, I);
        bool restriction = !eq(E, F) && !eq(E, H) &&
                           ((!eq(F, B) && !eq(H, D)) || (eq(E, I) && !eq(F, I4) && !eq(H, I5)) || eq(E, G) ||
                            eq(E, C));
        if (slash < backslash && restriction) {
            uint b = 1u;
            b |= dist(E, F) <= dist(E, H) ? 0u : 2u;
            b |= (2 * dist(F, G) <= dist(H, C) && !eq(E, G) && !eq(D, G)) ? 4u : 0u;
            b |= (dist(F, G) >= 2 * dist(H, C) && !eq(E, C) && !eq(B, C)) ? 8u : 0u;
            bits |= b << (4 * corner);
        }
    }
    return bits;
}
