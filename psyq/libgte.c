/* psyq/libgte.c: LIBGTE. Fixed point is the PS1's: 4096 = 1.0, a full turn = 4096. The functions that use the
 * GTE on the PS1 use the software GTE here (gte.c), with LIBGTE's command sequence, so they leave the GTE in the
 * console's state; the register setters write the GTE's control registers.
 *
 * Checked against the PS1 (session 16, the layer-1 family tests/golden/families/gte.py: LIBGTE's own functions in the
 * EXE, called in PCSX-Redux, replayed here by tests/host/gte_replay.py):
 *  - rsin/rcos: round-to-nearest of 4096 * sin(2 pi a / 4096) is Psy-Q's table, for a whole turn and outside it.
 *  - RotMatrixYXZ_gte is M = Ry * Rx * Rz and RotMatrixZYX_gte M = Rz * Ry * Rx with Psy-Q's right-handed matrices,
 *    but each product is floored on its own (see RotMatrixYXZ_gte): the exact product shifted once was off by one in
 *    about 80 of 96 cases.
 *  - ScaleMatrix multiplies on the CPU (32-bit products) and writes m[2][2]'s whole word: the pad halfword after it
 *    gets the product's high half (the shim used to keep it).
 *  - ApplyMatrixSV is MVMVA (sf 1, lm 0): IR saturated to s16.
 * LIBGS's helpers (MulMatrix, MulMatrix2, ApplyMatrixLV, TransposeMatrix, SquareRoot0) are checked by the family
 * tests/golden/families/libgs_view.py (tests/host/libgs_replay.py), with the GTE state each leaves.
 *
 * The rest of LIBGTE the second game calls (the end of this file: the perspective transforms, the matrix stack and
 * helpers, the normalisations, csqrt/catan/ratan2) issues the GTE commands LIBGTE's hand-written objects issue, in
 * their order, and stores what they store, so the results, FLAG and the GTE state left behind are the console's; the
 * tables are computed and were checked against the second game's EXE (psyq/README.md "Behaviour assumed": catan's
 * angles and what is not checked against the PS1 yet). */
#include <string.h>

#include "psyq_internal.h"
#include "psxstack/psyq/libgte.h"


/* ---- the sine table ---- */

static s16 psyq_sin_table[4096];
static int psyq_sin_ready;

/* sin(x) for 0 <= x <= pi/2 by its Taylor series (no libm: the shim must not add a link dependency). */
static double psyq_sin_series(double x) {
    double term = x;
    double sum = x;
    double x2 = x * x;
    int k;

    for (k = 1; k < 14; k++) {
        term *= -x2 / ((2.0 * k) * (2.0 * k + 1.0));
        sum += term;
    }
    return sum;
}

static void psyq_sin_init(void) {
    const double pi = 3.14159265358979323846;
    int i;

    for (i = 0; i <= 1024; i++) {
        double v = 4096.0 * psyq_sin_series(i * pi / 2048.0);
        s32 q = (s32)(v + 0.5); /* v >= 0 */

        if (q > 4096) {
            q = 4096;
        }
        psyq_sin_table[i] = (s16)q;                 /* 0 .. 90 degrees */
        psyq_sin_table[2048 - i] = (s16)q;          /* 180 - a */
        psyq_sin_table[(2048 + i) & 4095] = (s16)-q; /* 180 + a */
        psyq_sin_table[(4096 - i) & 4095] = (s16)-q; /* 360 - a */
    }
    psyq_sin_ready = 1;
}

/* Real: 4096 * sin(a), a in 1/4096 of a turn (any s32: it wraps). */
s32 rsin(s32 a) {
    if (!psyq_sin_ready) {
        psyq_sin_init();
    }
    return psyq_sin_table[a & 4095];
}

s32 rcos(s32 a) {
    if (!psyq_sin_ready) {
        psyq_sin_init();
    }
    return psyq_sin_table[(a + 1024) & 4095];
}

/* ---- matrices ---- */

/* Word i of a MATRIX (0..4: the rotation; word 4 is m[2][2] and the pad halfword after it), as lw reads it. */
static u32 psyq_mat_word(const MATRIX *m, int i) {
    u32 w;

    memcpy(&w, (const u8 *)m + 4 * i, 4);
    return w;
}

static void psyq_mat_set_word(MATRIX *m, int i, u32 w) {
    memcpy((u8 *)m + 4 * i, &w, 4);
}

static u32 psyq_pair(s32 lo, s32 hi) {
    return ((u32)lo & 0xFFFF) | ((u32)hi << 16);
}

/* IR0 = a, IR1-3 = b[0..2]; GPF (sf 1, lm 0); out = IR1-3 = (a * b) >> 12. Products of sines and cosines never
 * saturate. */
static void psyq_gpf3(s32 a, const s32 b[3], s32 out[3]) {
    int i;

    psyq_gte_mtc2(8, (u32)a);
    for (i = 0; i < 3; i++) {
        psyq_gte_mtc2(9 + i, (u32)b[i]);
    }
    psyq_gte_cmd(0x0198003D);
    for (i = 0; i < 3; i++) {
        out[i] = (s32)psyq_gte_mfc2(9 + i);
    }
}

/* RotMatrixYXZ_gte and RotMatrixZYX_gte: the rotation part of m from the angles, with LIBGTE's arithmetic: every
 * product of two sines/cosines is floored (>> 12: four GPF commands of three products each, as on the PS1, and two
 * CPU multiplies), a product of three is a floored product of a floored one, and the sums are taken last, in 16 bits.
 * The GPFs leave the GTE as LIBGTE leaves it. Words 0..3 of m are written whole and m[2][2] alone (the pad after it
 * and the translation are kept).
 * YXZ: m = Ry(vy) * Rx(vx) * Rz(vz), right-handed (Rz = [c -s 0; s c 0; 0 0 1], Rx = [1 0 0; 0 c -s; 0 s c],
 * Ry = [c 0 s; 0 1 0; -s 0 c]). */
MATRIX *RotMatrixYXZ_gte(SVECTOR *r, MATRIX *m) {
    s32 sx = rsin(r->vx), cx = rcos(r->vx);
    s32 sy = rsin(r->vy), cy = rcos(r->vy);
    s32 sz = rsin(r->vz), cz = rcos(r->vz);
    s32 in[3];
    s32 a[3]; /* cy * (sx, sz, cz) */
    s32 b[3]; /* sy * (sx, sz, cz) */
    s32 c[3]; /* cz * (cx, sy sx, cy sx) */
    s32 d[3]; /* sz * (cx, sy sx, cy sx) */

    in[0] = sx;
    in[1] = sz;
    in[2] = cz;
    psyq_gpf3(cy, in, a);
    psyq_gpf3(sy, in, b);
    in[0] = cx;
    in[1] = b[0];
    in[2] = a[0];
    psyq_gpf3(cz, in, c);
    psyq_gpf3(sz, in, d);
    m->m[2][2] = (s16)((cx * cy) >> 12);
    psyq_mat_set_word(m, 2, psyq_pair(c[0], -sx));                  /* m11, m12 */
    psyq_mat_set_word(m, 0, psyq_pair(a[2] + d[1], c[1] - a[1]));    /* m00, m01 */
    psyq_mat_set_word(m, 1, psyq_pair((cx * sy) >> 12, d[0]));       /* m02, m10 */
    psyq_mat_set_word(m, 3, psyq_pair(d[2] - b[2], c[2] + b[1]));    /* m20, m21 */
    return m;
}

/* ZYX: m = Rz(vz) * Ry(vy) * Rx(vx), the same arithmetic. */
MATRIX *RotMatrixZYX_gte(SVECTOR *r, MATRIX *m) {
    s32 sx = rsin(r->vx), cx = rcos(r->vx);
    s32 sy = rsin(r->vy), cy = rcos(r->vy);
    s32 sz = rsin(r->vz), cz = rcos(r->vz);
    s32 in[3];
    s32 a[3]; /* cx * (sy, sz, cz) */
    s32 b[3]; /* sx * (sy, sz, cz) */
    s32 c[3]; /* cz * (cy, sx sy, cx sy) */
    s32 d[3]; /* sz * (cy, sx sy, cx sy) */

    in[0] = sy;
    in[1] = sz;
    in[2] = cz;
    psyq_gpf3(cx, in, a);
    psyq_gpf3(sx, in, b);
    in[0] = cy;
    in[1] = b[0];
    in[2] = a[0];
    psyq_gpf3(cz, in, c);
    m->m[2][2] = (s16)((cy * cx) >> 12);
    psyq_mat_set_word(m, 3, psyq_pair(-sy, (cy * sx) >> 12));        /* m20, m21 */
    psyq_gpf3(sz, in, d);
    psyq_mat_set_word(m, 0, psyq_pair(c[0], c[1] - a[1]));           /* m00, m01 */
    psyq_mat_set_word(m, 1, psyq_pair(c[2] + b[1], d[0]));           /* m02, m10 */
    psyq_mat_set_word(m, 2, psyq_pair(d[1] + a[2], d[2] - b[2]));    /* m11, m12 */
    return m;
}

/* Column j of m scaled by v_j (m = m * diag(v)) on the CPU: each product is the low 32 bits of the 32 x 32 multiply,
 * shifted right by 12 with sign. Word 4 is written whole: the pad halfword after m[2][2] gets the shifted product's
 * high half. The translation is untouched. */
MATRIX *ScaleMatrix(MATRIX *m, VECTOR *v) {
    const s32 s[3] = { v->vx, v->vy, v->vz };
    int i;

    for (i = 0; i < 5; i++) {
        u32 w = psyq_mat_word(m, i);
        s32 lo = (s32)((u32)(s16)(w & 0xFFFF) * (u32)s[(2 * i) % 3]) >> 12;

        if (i == 4) {
            psyq_mat_set_word(m, 4, (u32)lo);
        } else {
            s32 hi = (s32)((u32)(s16)(w >> 16) * (u32)s[(2 * i + 1) % 3]) >> 12;

            psyq_mat_set_word(m, i, psyq_pair(lo, hi));
        }
    }
    return m;
}

/* v1 = (m * v0) >> 12 on the GTE, as LIBGTE does it: m's rotation becomes RT, v0 V0, MVMVA (sf 1, RT, V0, no
 * translation, lm 0), and IR1-3 (saturated to s16) go to v1's three components (its pad is kept). v1 may be v0. */
SVECTOR *ApplyMatrixSV(MATRIX *m, SVECTOR *v0, SVECTOR *v1) {
    u32 w;
    int i;

    for (i = 0; i < 5; i++) {
        psyq_gte_ctc2(i, psyq_mat_word(m, i));
    }
    memcpy(&w, v0, 4);
    psyq_gte_mtc2(0, w);
    memcpy(&w, (const u8 *)v0 + 4, 4);
    psyq_gte_mtc2(1, w);
    psyq_gte_cmd(0x00486012); /* MVMVA sf 1, mx RT, v V0, cv none, lm 0 */
    v1->vx = (s16)psyq_gte_mfc2(9);
    v1->vy = (s16)psyq_gte_mfc2(10);
    v1->vz = (s16)psyq_gte_mfc2(11);
    return v1;
}

/* ---- what LIBGS calls (GsSetRefView2, GsGetLw, GsMulCoord2/3), not the game ---- */

/* The rotation of m becomes RT (ctc2 0-4), as LIBGTE's matrix functions load it. */
static void psyq_load_rt(const MATRIX *m) {
    int i;

    for (i = 0; i < 5; i++) {
        psyq_gte_ctc2(i, psyq_mat_word(m, i));
    }
}

/* MulMatrix and MulMatrix2 (mtx_03.s, mtx_04.s): RT = m0's rotation, then each column of m1 through MVMVA (sf 1, RT,
 * V0, no translation, lm 0); IR1-3 (s16-saturated) are the product's column. Every word of m1 is read before the
 * first store, so out may be m0 or m1. The words are written as the PS1 writes them: word 4 is IR3's whole register
 * (swc2), so the pad halfword after m[2][2] gets IR3's sign. The translations are untouched. */
static void psyq_mul_matrix(const MATRIX *m0, const MATRIX *m1, MATRIX *out) {
    u32 w1 = psyq_mat_word(m1, 1), w2 = psyq_mat_word(m1, 2), w3 = psyq_mat_word(m1, 3), w4 = psyq_mat_word(m1, 4);
    u32 col[3][2];
    u32 ir[3][3];
    int c;
    int i;

    col[0][0] = (u16)m1->m[0][0] | (w1 & 0xFFFF0000u);
    col[0][1] = w3;
    col[1][0] = (u16)m1->m[0][1] | (w2 << 16);
    col[1][1] = (u32)(s32)m1->m[2][1];
    col[2][0] = (u16)m1->m[0][2] | (w2 & 0xFFFF0000u);
    col[2][1] = w4;
    psyq_load_rt(m0);
    for (c = 0; c < 3; c++) {
        psyq_gte_mtc2(0, col[c][0]);
        psyq_gte_mtc2(1, col[c][1]);
        psyq_gte_cmd(0x00486012); /* MVMVA sf 1, mx RT, v V0, cv none, lm 0 */
        for (i = 0; i < 3; i++) {
            ir[c][i] = psyq_gte_mfc2(9 + i);
        }
    }
    psyq_mat_set_word(out, 0, psyq_pair((s32)ir[0][0], (s32)ir[1][0]));
    psyq_mat_set_word(out, 3, psyq_pair((s32)ir[0][2], (s32)ir[1][2]));
    psyq_mat_set_word(out, 1, psyq_pair((s32)ir[2][0], (s32)ir[0][1]));
    psyq_mat_set_word(out, 2, psyq_pair((s32)ir[1][1], (s32)ir[2][1]));
    psyq_mat_set_word(out, 4, ir[2][2]);
}

/* m0 = m0 * m1 (rotations); returns m0. */
MATRIX *MulMatrix(MATRIX *m0, MATRIX *m1) {
    psyq_mul_matrix(m0, m1, m0);
    return m0;
}

/* m1 = m0 * m1 (rotations); returns m1. */
MATRIX *MulMatrix2(MATRIX *m0, MATRIX *m1) {
    psyq_mul_matrix(m0, m1, m1);
    return m1;
}

/* v1 = m * v0 for a 32-bit vector (mtx_004.s): each component is split by its magnitude into a high part (|v| >> 15)
 * and a low one (|v| & 0x7FFF), both with v's sign; MVMVA (sf 0) of the high parts, times 8 (<< 15 >> 12), plus MVMVA
 * (sf 1) of the low parts. MAC1-3 are read, not IR (no saturation). RT = m's rotation; v1 may be v0. */
VECTOR *ApplyMatrixLV(MATRIX *m, VECTOR *v0, VECTOR *v1) {
    const s32 in[3] = { v0->vx, v0->vy, v0->vz };
    u32 hi[3], lo[3], mac[3];
    int i;

    psyq_load_rt(m);
    for (i = 0; i < 3; i++) {
        u32 v = (u32)in[i];

        if (in[i] >= 0) {
            hi[i] = (u32)((s32)v >> 15);
            lo[i] = v & 0x7FFF;
        } else {
            v = 0u - v;
            hi[i] = 0u - (u32)((s32)v >> 15);
            lo[i] = 0u - (v & 0x7FFF);
        }
    }
    for (i = 0; i < 3; i++) {
        psyq_gte_mtc2(9 + i, hi[i]);
    }
    psyq_gte_cmd(0x0041E012); /* MVMVA sf 0, mx RT, v IR, cv none, lm 0 */
    for (i = 0; i < 3; i++) {
        mac[i] = psyq_gte_mfc2(25 + i) << 3;
    }
    for (i = 0; i < 3; i++) {
        psyq_gte_mtc2(9 + i, lo[i]);
    }
    psyq_gte_cmd(0x0049E012); /* MVMVA sf 1, mx RT, v IR, cv none, lm 0 */
    v1->vx = (s32)(psyq_gte_mfc2(25) + mac[0]);
    v1->vy = (s32)(psyq_gte_mfc2(26) + mac[1]);
    v1->vz = (s32)(psyq_gte_mfc2(27) + mac[2]);
    return v1;
}

/* m1's rotation = m0's transposed (fgo_00.s), with the PS1's word and halfword stores in their order, so m1 may be
 * m0. m1's translation is untouched. */
MATRIX *TransposeMatrix(MATRIX *m0, MATRIX *m1) {
    u8 *d = (u8 *)m1;
    const u8 *s = (const u8 *)m0;
    u32 t1, t2, t3;
    s16 h;

    memcpy(&t1, s + 0, 4);
    memcpy(&t2, s + 4, 4);
    memcpy(d + 4, &t1, 4);
    memcpy(d + 0, &t2, 4);
    memcpy(d + 0, &t1, 2); /* little-endian: the low halfword */
    memcpy(&t3, s + 8, 4);
    memcpy(&t1, s + 12, 4);
    memcpy(d + 12, &t3, 4);
    memcpy(d + 8, &t1, 4);
    memcpy(d + 12, &t2, 2);
    memcpy(d + 8, &t3, 2);
    memcpy(&h, s + 16, 2);
    memcpy(d + 4, &t1, 2);
    memcpy(d + 16, &h, 2);
    return m1;
}

/* LIBGTE's square-root table (D_800568A8): sqrt(i) * 512 rounded down, i = 64..255 (checked against the EXE's
 * table: all 192 entries). */
static u16 psyq_sqrt_table[192];

static void psyq_sqrt_init(void) {
    u32 i;

    for (i = 64; i < 256; i++) {
        u32 r = 0;

        while ((r + 1) * (r + 1) <= (i << 18)) {
            r++;
        }
        psyq_sqrt_table[i - 64] = (u16)r;
    }
}

/* sqrt(a) in integers (msc01.s): the GTE's leading-zero count (LZCS/LZCR) normalises a to 8 bits by an even shift,
 * the table gives its square root and the half shift scales it back. 0 for a = 0. The table index is kept in range
 * (the PS1 reads past the table for a negative a; the callers' sums of squares are not negative). */
s32 SquareRoot0(s32 a) {
    s32 lz, e, t, n;

    if (psyq_sqrt_table[0] == 0) {
        psyq_sqrt_init();
    }
    psyq_gte_mtc2(30, (u32)a);
    lz = (s32)psyq_gte_mfc2(31);
    if (lz == 32) {
        return 0;
    }
    e = lz & ~1;
    t = (31 - e) >> 1;
    n = e - 24;
    n = n >= 0 ? (s32)((u32)a << n) : a >> -n;
    return (s32)(((u32)psyq_sqrt_table[(u32)(n - 64) % 192] << t) >> 12);
}

/* ---- the GTE's control registers: LIBGTE's setters write them as the PS1's do ---- */

/* PushMatrix/PopMatrix's stack (LIBGTE's: 20 entries of RT and TR, the 8 control words 0..7). */
#define PSYQ_MATRIX_STACK 20
static u32 psyq_matrix_stack[PSYQ_MATRIX_STACK][8];
static s32 psyq_matrix_depth;

/* The console's reset (psyq.c psyq_reset): every GTE register zero, the matrix stack empty. The sine table is a cache
 * of a constant: kept. */
void psyq_gte_reset(void) {
    psyq_gte_clear();
    psyq_gte_shadow_reset();
    psyq_matrix_depth = 0;
    memset(psyq_matrix_stack, 0, sizeof(psyq_matrix_stack));
}

/* A save state (psyq_internal.h): LIBGTE's own state, the matrix stack (the GTE's registers are gte.c's). */
void psyq_gte_lib_state(PortState *s) {
    PORT_STATE_VAR(s, psyq_matrix_stack);
    PORT_STATE_VAR(s, psyq_matrix_depth);
}

/* ZSF3 = 0x155 and ZSF4 = 0x100 (1/3 and 1/4 for AVSZ3/4), H = 1000, DQA = -0x1062, DQB = 0x1400000, OFX = OFY = 0
 * (the PS1's also enables COP2: nothing to do here). */
void InitGeom(void) {
    PSYQ_TRACE("InitGeom");
    psyq_gte_ctc2(29, 0x155);
    psyq_gte_ctc2(30, 0x100);
    psyq_gte_ctc2(26, 0x3E8);
    psyq_gte_ctc2(27, (u32)-0x1062);
    psyq_gte_ctc2(28, 0x1400000);
    psyq_gte_ctc2(24, 0);
    psyq_gte_ctc2(25, 0);
}

/* OFX/OFY: the screen offset in 16.16. */
void SetGeomOffset(s32 ofx, s32 ofy) {
    PSYQ_TRACE("SetGeomOffset %d,%d", ofx, ofy);
    psyq_gte_ctc2(24, (u32)ofx << 16);
    psyq_gte_ctc2(25, (u32)ofy << 16);
}

/* H: the projection plane's distance (LIBGS's GsSetProjection calls it on the PS1). */
void SetGeomScreen(s32 h) {
    PSYQ_TRACE("SetGeomScreen %d", h);
    psyq_gte_ctc2(26, (u32)h);
}

/* BK: the back colour x 16. */
void SetBackColor(s32 rbk, s32 gbk, s32 bbk) {
    PSYQ_TRACE("SetBackColor %d,%d,%d", rbk, gbk, bbk);
    psyq_gte_ctc2(13, (u32)rbk << 4);
    psyq_gte_ctc2(14, (u32)gbk << 4);
    psyq_gte_ctc2(15, (u32)bbk << 4);
}

/* FC: the far colour x 16 (LIBGS's start-up, gte_init, calls it on the PS1). */
void SetFarColor(s32 rfc, s32 gfc, s32 bfc) {
    PSYQ_TRACE("SetFarColor %d,%d,%d", rfc, gfc, bfc);
    psyq_gte_ctc2(21, (u32)rfc << 4);
    psyq_gte_ctc2(22, (u32)gfc << 4);
    psyq_gte_ctc2(23, (u32)bfc << 4);
}

/* LCM: the light colour matrix, m's rotation words (LIBGS's GsSetFlatLight calls it on the PS1). */
void SetColorMatrix(MATRIX *m) {
    int i;

    PSYQ_TRACE("SetColorMatrix");
    for (i = 0; i < 5; i++) {
        psyq_gte_ctc2(16 + i, psyq_mat_word(m, i));
    }
}

/* ---- the rest of LIBGTE the second game calls ----
 * Each function below issues the commands of the PS1's (LIBGTE's hand-written MSC01, MTX_06, MTX_08, the perspective
 * transforms and PATCHGTE's NormalColorCol) in their order, with their stores, so FLAG, the results and the registers
 * left behind are the console's. The command words: */
#define PSYQ_GTE_RTPS 0x00180001u          /* sf 1 */
#define PSYQ_GTE_RTPT 0x00280030u          /* sf 1 */
#define PSYQ_GTE_NCLIP 0x01400006u
#define PSYQ_GTE_AVSZ3 0x0158002Du
#define PSYQ_GTE_AVSZ4 0x0168002Eu
#define PSYQ_GTE_NCCS 0x0108041Bu          /* sf 1, lm 1 */
#define PSYQ_GTE_NCCT 0x0118043Fu          /* sf 1, lm 1 */
#define PSYQ_GTE_MVMVA_RT_V0_TR 0x00480012u /* sf 1, mx RT, v V0, cv TR, lm 0 */
#define PSYQ_GTE_MVMVA_RT_V0 0x00486012u    /* sf 1, mx RT, v V0, cv none, lm 0 */
#define PSYQ_GTE_SQR0 0x00A00428u          /* sf 0, lm 1 */
#define PSYQ_GTE_OP1 0x0178000Cu           /* sf 1 */
#define PSYQ_GTE_GPF0 0x0190003Du          /* sf 0 */

/* lwc2: a word of the game's memory into data register `reg`. The stores to the game's memory are swc2s
 * (psyq_gte_swc2_, which also gives the sub-pixel shadow its SXY records, as the gte_* macros' stores do). */
static void psyq_lwc2(int reg, const void *p) {
    u32 w;

    memcpy(&w, p, 4);
    psyq_gte_mtc2(reg, w);
}

/* An SVECTOR into Vn (n = 0..2): its two words, VXYn = vx | vy << 16, VZn = vz. */
static void psyq_load_v(int n, const SVECTOR *v) {
    psyq_lwc2(2 * n, v);
    psyq_lwc2(2 * n + 1, (const u8 *)v + 4);
}

static void psyq_store_s32(s32 *p, u32 v) {
    memcpy(p, &v, 4);
}

static s32 psyq_flag(void) {
    return (s32)psyq_gte_cfc2(31);
}

/* SZ3 / 4: what the transforms return as the depth (otz). */
static s32 psyq_sz3_otz(void) {
    return (s32)psyq_gte_mfc2(19) >> 2;
}

/* ---- the register setters (MTX_08) ---- */

/* RT = m's rotation (control 0..4, its words as lw reads them). */
void SetRotMatrix(MATRIX *m) {
    psyq_load_rt(m);
}

/* LLM = m's rotation (control 8..12). */
void SetLightMatrix(MATRIX *m) {
    int i;

    for (i = 0; i < 5; i++) {
        psyq_gte_ctc2(8 + i, psyq_mat_word(m, i));
    }
}

/* TR = m's translation (control 5..7). */
void SetTransMatrix(MATRIX *m) {
    int i;

    for (i = 0; i < 3; i++) {
        psyq_gte_ctc2(5 + i, (u32)m->t[i]);
    }
}

/* m's translation = v's three words (MTX_06); returns m. */
MATRIX *TransMatrix(MATRIX *m, VECTOR *v) {
    m->t[0] = v->vx;
    m->t[1] = v->vy;
    m->t[2] = v->vz;
    return m;
}

/* ---- the matrix stack (MSC01) ---- */

/* RT and TR (control 0..7) onto LIBGTE's stack of 20; a full stack takes nothing (the PS1 prints a message to its TTY:
 * here a trace line). */
void PushMatrix(void) {
    int i;

    if (psyq_matrix_depth >= PSYQ_MATRIX_STACK) {
        PSYQ_TRACE("PushMatrix: the stack (%d entries) is full", PSYQ_MATRIX_STACK);
        return;
    }
    for (i = 0; i < 8; i++) {
        psyq_matrix_stack[psyq_matrix_depth][i] = psyq_gte_cfc2(i);
    }
    psyq_matrix_depth++;
}

/* RT and TR back from the stack; an empty stack changes nothing (the PS1 prints a message). */
void PopMatrix(void) {
    int i;

    if (psyq_matrix_depth <= 0) {
        PSYQ_TRACE("PopMatrix: the stack is empty");
        return;
    }
    psyq_matrix_depth--;
    for (i = 0; i < 8; i++) {
        psyq_gte_ctc2(i, psyq_matrix_stack[psyq_matrix_depth][i]);
    }
}

/* ---- matrices (MSC01, the CPU's RotMatrix/RotMatrixYXZ) ---- */

/* m2's rotation = m0's * m1's (MulMatrix's MVMVAs), returns m2; the translations are untouched. */
MATRIX *MulMatrix0(MATRIX *m0, MATRIX *m1, MATRIX *m2) {
    psyq_mul_matrix(m0, m1, m2);
    return m2;
}

/* m2 = m0 * m1 with translation: the rotation as MulMatrix0, then m2.t = m0.R * m1.t (MVMVA sf 1 into MAC1-3, not
 * saturated) + m0.t. m1's translation goes into V0 as LIBGTE loads it, its three components cut to 16 bits (the
 * documented limit: m1.t must fit a short). Returns m2; m2 may be m0 or m1. */
MATRIX *CompMatrix(MATRIX *m0, MATRIX *m1, MATRIX *m2) {
    s32 t0[3];
    int i;

    psyq_mul_matrix(m0, m1, m2);
    psyq_gte_mtc2(0, ((u32)m1->t[0] & 0xFFFF) | ((u32)m1->t[1] << 16));
    psyq_gte_mtc2(1, (u32)m1->t[2]);
    psyq_gte_cmd(PSYQ_GTE_MVMVA_RT_V0);
    for (i = 0; i < 3; i++) {
        t0[i] = m0->t[i];
    }
    for (i = 0; i < 3; i++) {
        m2->t[i] = (s32)(psyq_gte_mfc2(25 + i) + (u32)t0[i]);
    }
    return m2;
}

/* The product of two sines/cosines as the CPU's RotMatrix computes it: the low 32 bits, shifted right 12 with sign. */
static s32 psyq_fmul(s32 a, s32 b) {
    return (s32)((u32)a * (u32)b) >> 12;
}

/* m = Rx(vx) * Ry(vy) * Rz(vz) (LIBGTE's right-handed matrices, as RotMatrixYXZ_gte's comment shows them), computed on
 * the CPU as the PS1's RotMatrix does: the sine table is rsin/rcos's (checked: the EXE's sine/cosine table, all 4096
 * entries), every product floored on its own, a product of three a floored product of a floored one, negations where
 * LIBGTE takes them (before the shift in m[0][1] and m[1][2]). Only the nine halfwords are written. Returns m. */
MATRIX *RotMatrix(SVECTOR *r, MATRIX *m) {
    s32 sx = rsin(r->vx), cx = rcos(r->vx);
    s32 sy = rsin(r->vy), cy = rcos(r->vy);
    s32 sz = rsin(r->vz), cz = rcos(r->vz);
    s32 a = psyq_fmul(cz, -sy); /* cz (-sy) */
    s32 b = psyq_fmul(sz, -sy); /* sz (-sy) */

    m->m[0][0] = (s16)psyq_fmul(cz, cy);
    m->m[0][1] = (s16)((s32)(0u - (u32)sz * (u32)cy) >> 12);
    m->m[0][2] = (s16)sy;
    m->m[1][0] = (s16)(psyq_fmul(sz, cx) - psyq_fmul(a, sx));
    m->m[1][1] = (s16)(psyq_fmul(cz, cx) + psyq_fmul(b, sx));
    m->m[1][2] = (s16)((s32)(0u - (u32)cy * (u32)sx) >> 12);
    m->m[2][0] = (s16)(psyq_fmul(sz, sx) + psyq_fmul(a, cx));
    m->m[2][1] = (s16)(psyq_fmul(cz, sx) - psyq_fmul(b, cx));
    m->m[2][2] = (s16)psyq_fmul(cy, cx);
    return m;
}

/* m = Ry(vy) * Rx(vx) * Rz(vz) on the CPU (RotMatrixYXZ, not RotMatrixYXZ_gte), the same arithmetic: negations after
 * the shift here. Only the nine halfwords are written. Returns m. */
MATRIX *RotMatrixYXZ(SVECTOR *r, MATRIX *m) {
    s32 sx = rsin(r->vx), cx = rcos(r->vx);
    s32 sy = rsin(r->vy), cy = rcos(r->vy);
    s32 sz = rsin(r->vz), cz = rcos(r->vz);
    s32 a = psyq_fmul(sy, sx); /* sy sx */
    s32 b = psyq_fmul(cy, sx); /* cy sx */

    m->m[0][0] = (s16)(psyq_fmul(cy, cz) + psyq_fmul(a, sz));
    m->m[0][1] = (s16)(-psyq_fmul(cy, sz) + psyq_fmul(a, cz));
    m->m[0][2] = (s16)psyq_fmul(sy, cx);
    m->m[1][0] = (s16)psyq_fmul(sz, cx);
    m->m[1][1] = (s16)psyq_fmul(cz, cx);
    m->m[1][2] = (s16)-sx;
    m->m[2][0] = (s16)(-psyq_fmul(sy, cz) + psyq_fmul(b, sz));
    m->m[2][1] = (s16)(psyq_fmul(sy, sz) + psyq_fmul(b, cz));
    m->m[2][2] = (s16)psyq_fmul(cy, cx);
    return m;
}

/* ---- normalisation (MSC01) ---- */

/* LIBGTE's inverse-square-root table: floor(32768 / sqrt(i)), i = 64..255 (= the largest r with r * r * i <= 2^30;
 * checked against the second game's EXE: all 192 entries). */
static s16 psyq_isqrt_table[192];

static void psyq_isqrt_init(void) {
    u32 i;

    for (i = 64; i < 256; i++) {
        u32 r = 4096;

        while ((r + 1) * (r + 1) * i <= (1u << 30)) {
            r++;
        }
        while (r * r * i > (1u << 30)) {
            r--;
        }
        psyq_isqrt_table[i - 64] = (s16)r;
    }
}

/* v = v / |v| in 4.12, LIBGTE's way: the components go into IR1-3 (their low 16 bits: the function is meant for
 * vectors that fit), SQR (sf 0) squares them, the sum of the three is normalised to 8 bits by an even shift with the
 * leading-zero count (LZCS/LZCR), the table gives 1 / sqrt of it in IR0, GPF (sf 0) multiplies, and the half shift
 * scales MAC1-3 back (arithmetic shift, the amount's low 5 bits as srav takes them). A zero vector stays zero: the PS1
 * then reads the halfword 64 entries before the table, the square-root table's entry for 202 in this layout, into
 * IR0 (kept, for the GTE state); a sum past 31 bits (components near 2^15) has no meaning and is kept in range. */
static void psyq_normalize(s32 v[3]) {
    u32 sum, lz;
    s32 e, shift, k, n, idx;
    u32 ir0;
    int i;

    if (psyq_isqrt_table[0] == 0) {
        psyq_isqrt_init();
    }
    if (psyq_sqrt_table[0] == 0) {
        psyq_sqrt_init();
    }
    for (i = 0; i < 3; i++) {
        psyq_gte_mtc2(9 + i, (u32)v[i]);
    }
    psyq_gte_cmd(PSYQ_GTE_SQR0);
    sum = psyq_gte_mfc2(25) + psyq_gte_mfc2(26) + psyq_gte_mfc2(27);
    psyq_gte_mtc2(30, sum);
    lz = psyq_gte_mfc2(31);
    e = (s32)(lz & ~1u);
    shift = (31 - e) >> 1;
    k = e - 24;
    n = k >= 0 ? (s32)(sum << k) : (s32)sum >> (24 - e);
    idx = n - 64;
    if (idx == -64) {
        ir0 = psyq_sqrt_table[202 - 64];
    } else {
        ir0 = (u32)(s32)psyq_isqrt_table[(u32)idx % 192];
    }
    psyq_gte_mtc2(8, ir0);
    for (i = 0; i < 3; i++) {
        psyq_gte_mtc2(9 + i, (u32)v[i]);
    }
    psyq_gte_cmd(PSYQ_GTE_GPF0);
    for (i = 0; i < 3; i++) {
        v[i] = (s32)psyq_gte_mfc2(25 + i) >> (shift & 31);
    }
}

/* v1 = v0 / |v0| in 4.12 (three words; v1's pad is kept). */
void VectorNormal(VECTOR *v0, VECTOR *v1) {
    s32 v[3];

    v[0] = v0->vx;
    v[1] = v0->vy;
    v[2] = v0->vz;
    psyq_normalize(v);
    v1->vx = v[0];
    v1->vy = v[1];
    v1->vz = v[2];
}

/* n's rotation = m's made orthonormal, as LIBGTE does it with OP: r2 = r0 x r1 (OP sf 1 with RT's diagonal = row 0,
 * IR = row 1), r0' = r1 x r2 (OP again, IR = the first product saturated), each row then normalised; n = [r0'; r1;
 * r2]. RT's diagonal words (control 0, 2, 4) are put back; V0/V1 hold row 1 (LIBGTE keeps it there). Only the nine
 * halfwords of n are written. */
void MatrixNormal(MATRIX *m, MATRIX *n) {
    s32 r0[3], r1[3], a[3], b[3], c[3];
    u32 rt0, rt2, rt4;
    int i;

    for (i = 0; i < 3; i++) {
        r0[i] = m->m[0][i];
        r1[i] = m->m[1][i];
    }
    rt0 = psyq_gte_cfc2(0);
    rt2 = psyq_gte_cfc2(2);
    rt4 = psyq_gte_cfc2(4);
    psyq_gte_ctc2(0, (u32)r0[0]);
    psyq_gte_ctc2(2, (u32)r0[1]);
    psyq_gte_ctc2(4, (u32)r0[2]);
    psyq_gte_mtc2(11, (u32)r1[2]);
    psyq_gte_mtc2(9, (u32)r1[0]);
    psyq_gte_mtc2(10, (u32)r1[1]);
    psyq_gte_cmd(PSYQ_GTE_OP1);
    for (i = 0; i < 3; i++) {
        c[i] = (s32)psyq_gte_mfc2(25 + i);
    }
    psyq_gte_ctc2(0, (u32)r1[0]);
    psyq_gte_ctc2(2, (u32)r1[1]);
    psyq_gte_ctc2(4, (u32)r1[2]);
    psyq_gte_cmd(PSYQ_GTE_OP1);
    for (i = 0; i < 3; i++) {
        psyq_gte_mtc2(i, (u32)r1[i]);
    }
    for (i = 0; i < 3; i++) {
        a[i] = (s32)psyq_gte_mfc2(25 + i);
    }
    psyq_gte_ctc2(0, rt0);
    psyq_gte_ctc2(2, rt2);
    psyq_gte_ctc2(4, rt4);
    psyq_normalize(a);
    for (i = 0; i < 3; i++) {
        n->m[0][i] = (s16)a[i];
    }
    for (i = 0; i < 3; i++) {
        b[i] = (s32)psyq_gte_mfc2(i);
    }
    psyq_normalize(b);
    for (i = 0; i < 3; i++) {
        n->m[1][i] = (s16)b[i];
    }
    psyq_normalize(c);
    for (i = 0; i < 3; i++) {
        n->m[2][i] = (s16)c[i];
    }
}

/* ---- the perspective transforms ----
 * sxy: SX, SY as one word; p: IR0 (the depth-cue factor); flag: FLAG; otz: SZ3 / 4 (or AVSZ3/4's OTZ); the return is
 * the depth, OTZ or the NCLIP product. The four-vertex forms run RTPT on v0..v2, then RTPS on v3, and OR the two
 * FLAGs. The Nclip forms store nothing but *flag when the outer product (MAC0) is <= 0, and return it. */

/* RTPS of v0. Returns SZ3 / 4. */
s32 RotTransPers(SVECTOR *v0, s32 *sxy, s32 *p, s32 *flag) {
    psyq_load_v(0, v0);
    psyq_gte_cmd(PSYQ_GTE_RTPS);
    psyq_gte_swc2_(sxy, 14);
    psyq_gte_swc2_(p, 8);
    psyq_store_s32(flag, (u32)psyq_flag());
    return psyq_sz3_otz();
}

/* RTPT of v0..v2. Returns SZ3 / 4 (of v2). */
s32 RotTransPers3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *p, s32 *flag) {
    psyq_load_v(0, v0);
    psyq_load_v(1, v1);
    psyq_load_v(2, v2);
    psyq_gte_cmd(PSYQ_GTE_RTPT);
    psyq_gte_swc2_(sxy0, 12);
    psyq_gte_swc2_(sxy1, 13);
    psyq_gte_swc2_(sxy2, 14);
    psyq_gte_swc2_(p, 8);
    psyq_store_s32(flag, (u32)psyq_flag());
    return psyq_sz3_otz();
}

/* v1 = RT * v0 + TR (MVMVA sf 1: MAC1-3, not saturated); no perspective. */
void RotTrans(SVECTOR *v0, VECTOR *v1, s32 *flag) {
    psyq_load_v(0, v0);
    psyq_gte_cmd(PSYQ_GTE_MVMVA_RT_V0_TR);
    psyq_gte_swc2_(&v1->vx, 25);
    psyq_gte_swc2_(&v1->vy, 26);
    psyq_gte_swc2_(&v1->vz, 27);
    psyq_store_s32(flag, (u32)psyq_flag());
}

/* RTPT of v0..v2 and their SXY, then RTPS of v3 (its SXY, IR0 as p). */
static u32 psyq_rot_trans_pers4(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, SVECTOR *v3, s32 *sxy0, s32 *sxy1, s32 *sxy2,
                                s32 *sxy3, s32 *p) {
    u32 f;

    psyq_load_v(0, v0);
    psyq_load_v(1, v1);
    psyq_load_v(2, v2);
    psyq_gte_cmd(PSYQ_GTE_RTPT);
    psyq_gte_swc2_(sxy0, 12);
    psyq_gte_swc2_(sxy1, 13);
    psyq_gte_swc2_(sxy2, 14);
    f = (u32)psyq_flag();
    psyq_load_v(0, v3);
    psyq_gte_cmd(PSYQ_GTE_RTPS);
    psyq_gte_swc2_(sxy3, 14);
    psyq_gte_swc2_(p, 8);
    return f | (u32)psyq_flag();
}

/* Returns SZ3 / 4 (of v3). */
s32 RotTransPers4(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, SVECTOR *v3, s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *sxy3,
                  s32 *p, s32 *flag) {
    psyq_store_s32(flag, psyq_rot_trans_pers4(v0, v1, v2, v3, sxy0, sxy1, sxy2, sxy3, p));
    return psyq_sz3_otz();
}

/* RotTransPers4, then AVSZ4 (after *flag is stored: its FLAG is not in it). Returns OTZ. */
s32 RotAverage4(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, SVECTOR *v3, s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *sxy3,
                s32 *p, s32 *flag) {
    psyq_store_s32(flag, psyq_rot_trans_pers4(v0, v1, v2, v3, sxy0, sxy1, sxy2, sxy3, p));
    psyq_gte_cmd(PSYQ_GTE_AVSZ4);
    return (s32)psyq_gte_mfc2(7);
}

/* RTPT of v0..v2 and NCLIP; *flag = RTPT's FLAG. Returns MAC0 (the outer product). */
static s32 psyq_rot_nclip3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, s32 *flag, u32 *f) {
    psyq_load_v(0, v0);
    psyq_load_v(1, v1);
    psyq_load_v(2, v2);
    psyq_gte_cmd(PSYQ_GTE_RTPT);
    *f = (u32)psyq_flag();
    psyq_store_s32(flag, *f);
    psyq_gte_cmd(PSYQ_GTE_NCLIP);
    return (s32)psyq_gte_mfc2(24);
}

static void psyq_store_sxy3(s32 *sxy0, s32 *sxy1, s32 *sxy2) {
    psyq_gte_swc2_(sxy0, 12);
    psyq_gte_swc2_(sxy1, 13);
    psyq_gte_swc2_(sxy2, 14);
}

/* Front-facing (MAC0 > 0): the SXYs, p and *otz = SZ3 / 4. Returns MAC0. */
s32 RotNclip3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *p, s32 *otz, s32 *flag) {
    u32 f;
    s32 opz = psyq_rot_nclip3(v0, v1, v2, flag, &f);

    if (opz > 0) {
        psyq_store_sxy3(sxy0, sxy1, sxy2);
        psyq_gte_swc2_(p, 8);
        psyq_store_s32(otz, (u32)psyq_sz3_otz());
    }
    return opz;
}

/* Front-facing: v0..v2's SXYs, RTPS of v3 (its SXY, p), *otz = SZ3 / 4, *flag = both FLAGs; LIBGTE then returns that
 * flag word, not MAC0 (its return register is reused). Back-facing: returns MAC0. */
s32 RotNclip4(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, SVECTOR *v3, s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *sxy3, s32 *p,
              s32 *otz, s32 *flag) {
    u32 f, f2;
    s32 opz = psyq_rot_nclip3(v0, v1, v2, flag, &f);

    if (opz <= 0) {
        return opz;
    }
    psyq_store_sxy3(sxy0, sxy1, sxy2);
    psyq_load_v(0, v3);
    psyq_gte_cmd(PSYQ_GTE_RTPS);
    psyq_gte_swc2_(sxy3, 14);
    psyq_gte_swc2_(p, 8);
    psyq_store_s32(otz, (u32)psyq_sz3_otz());
    f2 = (u32)psyq_flag() | f;
    psyq_store_s32(flag, f2);
    return (s32)f2;
}

/* Front-facing: the SXYs, p, AVSZ3 and *otz = OTZ. Returns MAC0. */
s32 RotAverageNclip3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *p, s32 *otz,
                     s32 *flag) {
    u32 f;
    s32 opz = psyq_rot_nclip3(v0, v1, v2, flag, &f);

    if (opz > 0) {
        psyq_store_sxy3(sxy0, sxy1, sxy2);
        psyq_gte_swc2_(p, 8);
        psyq_gte_cmd(PSYQ_GTE_AVSZ3);
        psyq_store_s32(otz, psyq_gte_mfc2(7));
    }
    return opz;
}

/* Front-facing: v0..v2's SXYs, RTPS of v3 (its SXY, p), *flag = both FLAGs, AVSZ4 and *otz = OTZ. Returns MAC0. */
s32 RotAverageNclip4(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, SVECTOR *v3, s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *sxy3,
                     s32 *p, s32 *otz, s32 *flag) {
    u32 f;
    s32 opz = psyq_rot_nclip3(v0, v1, v2, flag, &f);

    if (opz > 0) {
        psyq_store_sxy3(sxy0, sxy1, sxy2);
        psyq_load_v(0, v3);
        psyq_gte_cmd(PSYQ_GTE_RTPS);
        psyq_gte_swc2_(sxy3, 14);
        f |= (u32)psyq_flag();
        psyq_gte_swc2_(p, 8);
        psyq_store_s32(flag, f);
        psyq_gte_cmd(PSYQ_GTE_AVSZ4);
        psyq_store_s32(otz, psyq_gte_mfc2(7));
    }
    return opz;
}

/* ---- lighting (PATCHGTE) ---- */

/* NCCS: the normal v0 lit (LLM, LCM, BK) times the colour v1 (RGBC, its code byte kept); the result is RGB2. */
void NormalColorCol(SVECTOR *v0, CVECTOR *v1, CVECTOR *v2) {
    psyq_load_v(0, v0);
    psyq_lwc2(6, v1);
    psyq_gte_cmd(PSYQ_GTE_NCCS);
    psyq_gte_swc2_(v2, 22);
}

/* NCCT: the three normals with one colour v3; the results are RGB0..2. */
void NormalColorCol3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, CVECTOR *v3, CVECTOR *v4, CVECTOR *v5, CVECTOR *v6) {
    psyq_load_v(0, v0);
    psyq_load_v(1, v1);
    psyq_load_v(2, v2);
    psyq_lwc2(6, v3);
    psyq_gte_cmd(PSYQ_GTE_NCCT);
    psyq_gte_swc2_(v4, 20);
    psyq_gte_swc2_(v5, 21);
    psyq_gte_swc2_(v6, 22);
}

/* ---- scalar maths (GEO_01, FGO_00) ----
 * atan in double without libm (the shim adds no link dependency): the series of atan(y) for |y| <= tan(pi / 8), and
 * atan(x) = pi / 4 + atan((x - 1) / (x + 1)) above it; only the tables below use it (x in 0..1.0004). */
static double psyq_atan_series(double y) {
    double y2 = y * y;
    double term = y;
    double sum = y;
    int k;

    for (k = 1; k < 40; k++) {
        term *= -y2;
        sum += term / (2.0 * k + 1.0);
    }
    return sum;
}

static double psyq_atan(double x) {
    const double pi = 3.14159265358979323846;

    if (x > 0.41421356237309503) {
        return pi / 4.0 + psyq_atan_series((x - 1.0) / (x + 1.0));
    }
    return psyq_atan_series(x);
}

/* csqrt: sqrt(a) in 20.12 fixed point (sqrt(a * 4096)), LIBGTE's way: the leading-zero count (the GTE's LZCS/LZCR)
 * scales a by an even power of two into 2^22..2^24, a hyperbolic CORDIC (vectoring x = m + c, y = m - c through the
 * shifts 1, 2, 3, 4, 4, 5, 6, signed arithmetic shifts) gives 4096 * sqrt(m) in x, and the half shift scales it back.
 * c = floor(2^22 / Kh^2), Kh the hyperbolic CORDIC gain (the product of sqrt(1 - 2^-2i) over i >= 1 with 4, 13 and
 * 40 taken twice: 0.82815936...), so that x ends as 2 Kh sqrt(m c) = 4096 sqrt(m); this is LIBGTE's constant (checked
 * against the second game's EXE). Not exact: csqrt(0x10000) = 0x4000, csqrt(0x1000000) is 4 * 65536 + 16. */
static s32 psyq_csqrt_c;

static s32 psyq_csqrt_cordic(s32 m) {
    static const int shifts[7] = { 1, 2, 3, 4, 4, 5, 6 };
    s32 x = (s32)((u32)m + (u32)psyq_csqrt_c);
    s32 y = (s32)((u32)m - (u32)psyq_csqrt_c);
    int k;

    for (k = 0; k < 7; k++) {
        int i = shifts[k];
        s32 nx, ny;

        if (y >= 0) {
            nx = (s32)((u32)x - (u32)(y >> i));
            ny = (s32)((u32)y - (u32)(x >> i));
        } else {
            nx = (s32)((u32)x + (u32)(y >> i));
            ny = (s32)((u32)y + (u32)(x >> i));
        }
        x = nx;
        y = ny;
    }
    return x;
}

s32 csqrt(s32 a) {
    s32 n, s, m, r;

    if (psyq_csqrt_c == 0) {
        double prod = 1.0, q = 1.0;
        int i;

        for (i = 1; i <= 60; i++) {
            q /= 4.0;
            prod *= 1.0 - q;
            if (i == 4 || i == 13 || i == 40) {
                prod *= 1.0 - q;
            }
        }
        psyq_csqrt_c = (s32)(4194304.0 / prod);
    }
    if (a == 0) {
        return 0;
    }
    psyq_gte_mtc2(30, (u32)a);
    n = 8 - (s32)psyq_gte_mfc2(31);
    if (n >= 0) {
        s = n >> 1;
        m = a >> ((2 * s) & 31);
    } else {
        s = (n >> 1) + 1;
        m = (s32)((u32)a << ((-2 * s) & 31));
    }
    s -= 6;
    r = psyq_csqrt_cordic(m);
    return s >= 0 ? (s32)((u32)r << (s & 31)) : r >> ((-s) & 31);
}

/* catan: atan(a / 4096) in 4096 per turn, a circular CORDIC from (4096, a): 12 rotations by atan(2^-i) (arithmetic
 * shifts), the angles summed. The angles here are round(atan(2^-i) * 2048 / pi); LIBGTE's own table differs from
 * those at four of the twelve entries and could not be derived from a formula, so this catan differs from the PS1's by
 * 1 or 3 (psyq/README.md "Behaviour assumed"). */
static s32 psyq_catan_angle[12];

s32 catan(s32 a) {
    s32 x = 4096, y = a, z = 0;
    int i;

    if (psyq_catan_angle[0] == 0) {
        const double pi = 3.14159265358979323846;
        double t = 1.0;

        for (i = 0; i < 12; i++) {
            psyq_catan_angle[i] = (s32)(psyq_atan(t) * 2048.0 / pi + 0.5);
            t /= 2.0;
        }
    }
    for (i = 0; i < 12; i++) {
        s32 nx, ny;

        if (y < 0) {
            nx = (s32)((u32)x - (u32)(y >> i));
            ny = (s32)((u32)y + (u32)(x >> i));
            z -= psyq_catan_angle[i];
        } else {
            nx = (s32)((u32)x + (u32)(y >> i));
            ny = (s32)((u32)y - (u32)(x >> i));
            z += psyq_catan_angle[i];
        }
        x = nx;
        y = ny;
    }
    return z;
}

/* ratan2's table: round(atan((i + 3/8) / 1024) * 2048 / pi), i = 0..1024 (the angle of the slope i / 1024 in 4096 per
 * turn; LIBGTE's table is exactly this: checked against the second game's EXE, all 1025 entries; the nearest entry is
 * 0.0009 from a rounding boundary, far above the series' error). */
static s16 psyq_atan_table[1025];
static int psyq_atan_ready;

/* a / b as the R3000's div leaves LO: truncated, -1 (a >= 0) or 1 (a < 0) for b = 0, and -2^31 / -1 = -2^31. */
static s32 psyq_div(s32 a, s32 b) {
    if (b == 0) {
        return a >= 0 ? -1 : 1;
    }
    if (b == -1) {
        return (s32)(0u - (u32)a);
    }
    return a / b;
}

/* ratan2: atan2(y, x) in 4096 per turn (-2048..2048): the table at the smaller magnitude over the larger one in 1/1024,
 * mirrored by octant; magnitudes of 2^21 and more are divided by (larger >> 10) instead of multiplying the smaller by
 * 1024 (LIBGTE's way, so the same rounding). 0 for (0, 0). An index outside the table (only for -2^31, whose magnitude
 * stays negative: the PS1 reads past the table) is clamped. */
s32 ratan2(s32 y, s32 x) {
    int neg_x = 0, neg_y = 0;
    s32 i, ret;

    if (!psyq_atan_ready) {
        const double pi = 3.14159265358979323846;
        int k;

        for (k = 0; k <= 1024; k++) {
            psyq_atan_table[k] = (s16)(psyq_atan((k + 0.375) / 1024.0) * 2048.0 / pi + 0.5);
        }
        psyq_atan_ready = 1;
    }
    if (x < 0) {
        neg_x = 1;
        x = (s32)(0u - (u32)x);
    }
    if (y < 0) {
        neg_y = 1;
        y = (s32)(0u - (u32)y);
    }
    if (x == 0 && y == 0) {
        return 0;
    }
    if (y < x) {
        i = (y & 0x7FE00000) ? psyq_div(y, x >> 10) : psyq_div((s32)((u32)y << 10), x);
        ret = psyq_atan_table[i < 0 ? 0 : i > 1024 ? 1024 : i];
    } else {
        i = (x & 0x7FE00000) ? psyq_div(x, y >> 10) : psyq_div((s32)((u32)x << 10), y);
        ret = 0x400 - psyq_atan_table[i < 0 ? 0 : i > 1024 ? 1024 : i];
    }
    if (neg_x) {
        ret = 0x800 - ret;
    }
    if (neg_y) {
        ret = -ret;
    }
    return ret;
}
