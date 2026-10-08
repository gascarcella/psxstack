/* runtime/render_gpu_subpixel.c: sub-pixel precision in the hardware renderer (docs/PORT.md "Sub-pixel precision").
 * Above internal scale 1, a triangle whose vertices gpu.c's listener reports with the GTE shadow's fraction
 * (psyq/gte_shadow.c: GpuVertex fx, fy) is drawn at those positions instead of the PS1's whole pixels, so the 3D moves
 * smoothly instead of a whole 1x pixel at a time. A precise vertex is placed at x + fx / 65536 - 1/2: the integer the
 * PS1 draws is the floor of the precise value, so the half pixel centres it on the integer, and a vertex without a
 * value (2D, clamped, the shadow off) stays where it was. Its attributes cannot come from render_gpu.c's integer
 * plane equations, which need whole-pixel vertices: this file computes float planes (raster.frag.hlsl F_PRECISE), the
 * same sample point and rounding as the integer path's (an attribute at (P + 1/2) / N - 1/2, rounded to the nearest).
 * Nothing here changes what gpu.c draws: the software picture, the stream and internal scale 1 are untouched. */
#ifdef PSXSTACK_SDL
#include <math.h>
#include <string.h>

#include "port_harness.h"
#include "render_gpu.h"
#include "render_gpu_subpixel.h"

static int subpixel_on = 1; /* video.subpixel / --subpixel */
static int subpixel_active;  /* the shadow is on for the rasteriser */

void render_gpu_set_subpixel(int on) {
    subpixel_on = on != 0;
}

void render_subpixel_start(int scale) {
    subpixel_active = subpixel_on && scale > 1;
    if (subpixel_active) {
        psyq_gte_shadow_enable(1);
        port_log("renderer: gpu: sub-pixel vertices on (video.subpixel)");
    }
}

void render_subpixel_stop(void) {
    PsyqShadowStats s;

    if (!subpixel_active) {
        return;
    }
    psyq_gte_shadow_stats(&s);
    port_log("renderer: gpu: sub-pixel: %ld polygon vertices drawn, %ld at their sub-pixel position (%.1f %%; %ld at a "
             "shared word's mean)",
             s.drawn, s.precise, s.drawn > 0 ? 100.0 * (double)s.precise / (double)s.drawn : 0.0, s.ambiguous);
    psyq_gte_shadow_enable(0);
    subpixel_active = 0;
}

/* The plane through (x_i, y_i, value_i): (value at vertex 0, d/dx, d/dy); flat when the triangle has no area. */
static void plane(float out[4], const double x[3], const double y[3], const double val[3]) {
    double dx1 = x[1] - x[0], dy1 = y[1] - y[0], dx2 = x[2] - x[0], dy2 = y[2] - y[0];
    double den = dx1 * dy2 - dx2 * dy1, e1 = val[1] - val[0], e2 = val[2] - val[0];

    out[0] = (float)val[0];
    out[1] = out[2] = out[3] = 0.0f;
    if (fabs(den) > 1e-9) {
        out[1] = (float)((e1 * dy2 - e2 * dy1) / den);
        out[2] = (float)((e2 * dx1 - e1 * dx2) / den);
    }
}

int render_subpixel_triangle(const GpuEvent *ev, float pos[3][2], float planes[SUBPIXEL_PLANES][4]) {
    double x[3], y[3], q[3], a[5][3];
    int i, k, any = 0;

    if (!subpixel_active) {
        return 0;
    }
    for (i = 0; i < 3; i++) {
        const GpuVertex *v = &ev->v[i];
        int precise = v->fx >= 0 && v->fy >= 0;

        any |= precise;
        x[i] = v->x + (precise ? v->fx / 65536.0 - 0.5 : 0.0);
        y[i] = v->y + (precise ? v->fy / 65536.0 - 0.5 : 0.0);
        q[i] = 1.0;
        a[0][i] = v->u;
        a[1][i] = v->v;
        a[2][i] = v->r;
        a[3][i] = v->g;
        a[4][i] = v->b;
    }
    if (!any) {
        return 0;
    }
    for (i = 0; i < 3; i++) {
        pos[i][0] = (float)x[i];
        pos[i][1] = (float)y[i];
    }
    memset(planes, 0, sizeof(float) * SUBPIXEL_PLANES * 4);
    planes[0][0] = (float)x[0];
    planes[0][1] = (float)y[0];
    plane(planes[1], x, y, q);
    for (k = 0; k < 5; k++) {
        double aq[3];
        for (i = 0; i < 3; i++) {
            aq[i] = a[k][i] * q[i];
        }
        plane(planes[2 + k], x, y, aq);
    }
    return 1;
}
#endif
