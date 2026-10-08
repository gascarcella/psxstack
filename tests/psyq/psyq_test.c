/* tests/psyq/psyq_test.c: the shim's LIBGTE, LIBGPU, LIBGS and LIBC2 functions on their own (tests/psyq_test.py builds
 * this with the shim's sources and the flags of psyq/check.sh, and runs it). Documented cases and properties, no game
 * and no disc: the values a function must give by its definition (Sony's descriptions, psx-spx), a function against the
 * commands it is made of, and the packets' bytes. What only the PS1 can settle (the exact rounding of the CORDICs, FLAG
 * in corner cases) is the consumers' goldens' (psyq/README.md "Behaviour assumed"). Prints one line per failure; exit 1
 * on any. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psyq_internal.h"
#include "psxstack/spu.h"
#include "psxstack/psyq/libgpu.h"
#include "psxstack/psyq/libgte.h"
#include "psxstack/psyq/libgs.h"
#include "libgs_internal.h"

/* ---- what the runtime gives the shim: the tag window (a static arena here), the rest unused ---- */

static u32 arena[1 << 16];
const uint8_t *port_tag_base = (const uint8_t *)arena;
uint32_t port_tag_span = sizeof(arena);

uint32_t port_ptr_to_u32(const void *p) {
    uintptr_t off = (uintptr_t)p - (uintptr_t)arena;

    if ((const u8 *)p < (const u8 *)arena || off >= sizeof(arena) || (off & 3) != 0) {
        fprintf(stderr, "port_ptr_to_u32: %p is not in the tag window\n", p);
        exit(2);
    }
    return (uint32_t)(off >> PORT_TAG_SHIFT);
}

void port_unimplemented(const char *what) {
    fprintf(stderr, "port_unimplemented: %s\n", what);
    exit(2);
}

/* The SPU core (runtime/spu.c), which LIBSND and LIBCD write: not under test. */
void spu_write16(uint32_t offset, uint16_t value) {
    (void)offset;
    (void)value;
}

uint16_t spu_read16(uint32_t offset) {
    (void)offset;
    return 0;
}

void spu_dma_write(const uint16_t *data, uint32_t halfwords) {
    (void)data;
    (void)halfwords;
}

void spu_cd_input(const int16_t *samples, int frames) {
    (void)samples;
    (void)frames;
}

/* ---- checks ---- */

static int failures, checks;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        checks++;                                            \
        if (!(cond) && ++failures <= 40) {                   \
            printf("FAIL %s:%d: ", __func__, __LINE__);      \
            printf(__VA_ARGS__);                             \
            putchar('\n');                                   \
        }                                                    \
    } while (0)

static const double PI = 3.14159265358979323846;

/* RT = identity-ish rotation about y by `angle`, TR, H, OFX/OFY: a camera for the transforms. */
static void setup_camera(s32 angle) {
    SVECTOR r = { 0, (s16)angle, 0, 0 };
    MATRIX m;

    memset(&m, 0, sizeof(m));
    RotMatrix(&r, &m);
    m.t[0] = 10;
    m.t[1] = -20;
    m.t[2] = 1000;
    SetRotMatrix(&m);
    SetTransMatrix(&m);
    InitGeom();
    SetGeomOffset(160, 120);
    SetGeomScreen(512);
}

/* ---- LIBGTE ---- */

static void test_scalar(void) {
    s32 a, y, x;
    s32 worst = 0;

    /* csqrt: 20.12 in, 20.12 out (sqrt(16.0) = 4.0); SquareRoot0: an integer square root. */
    CHECK(csqrt(0) == 0, "csqrt(0) = %d", csqrt(0));
    CHECK(csqrt(0x10000) == 0x4000, "csqrt(0x10000) = %#x, want 0x4000 (sqrt(16.0) = 4.0)", csqrt(0x10000));
    CHECK(csqrt(0x1000) == 0x1000, "csqrt(1.0) = %#x", csqrt(0x1000));
    CHECK(SquareRoot0(0x10000) == 0x100, "SquareRoot0(0x10000) = %#x", SquareRoot0(0x10000));
    for (a = 1; a < 0x7FFFFFFF - 9973 * 7919; a += 9973 * 7919 / 64 + 1) {
        double want = sqrt((double)a * 4096.0);
        double got = csqrt(a);

        /* LIBGTE's CORDIC has seven steps: within 0.02 % (and 2 units). */
        CHECK(fabs(got - want) <= 2.0 + want * 2e-4, "csqrt(%d) = %.0f, sqrt = %.1f", a, got, want);
    }
    /* ratan2: atan2 in 4096 per turn, exact at the axes and diagonals, within 1 elsewhere. */
    CHECK(ratan2(0, 0) == 0, "ratan2(0, 0) = %d", ratan2(0, 0));
    CHECK(ratan2(0, 100) == 0, "ratan2(0, 100) = %d", ratan2(0, 100));
    CHECK(ratan2(100, 100) == 512, "ratan2(100, 100) = %d", ratan2(100, 100));
    CHECK(ratan2(100, 0) == 1024, "ratan2(100, 0) = %d", ratan2(100, 0));
    CHECK(ratan2(0, -100) == 2048, "ratan2(0, -100) = %d", ratan2(0, -100));
    CHECK(ratan2(-100, 0) == -1024, "ratan2(-100, 0) = %d", ratan2(-100, 0));
    CHECK(ratan2(-100, -100) == -1536, "ratan2(-100, -100) = %d", ratan2(-100, -100));
    for (y = -5000; y <= 5000; y += 37) {
        for (x = -5000; x <= 5000; x += 41) {
            s32 want = (s32)lround(atan2((double)y, (double)x) * 2048.0 / PI);
            s32 d = ratan2(y, x) - want;

            if (d < 0) {
                d = -d;
            }
            if (d > worst) {
                worst = d;
            }
        }
    }
    CHECK(worst <= 1, "ratan2 is %d from atan2 at worst", worst);
    CHECK(ratan2(3 << 22, 4 << 22) == ratan2(3, 4) || abs(ratan2(3 << 22, 4 << 22) - ratan2(3, 4)) <= 1,
          "ratan2 of large values: %d vs %d", ratan2(3 << 22, 4 << 22), ratan2(3, 4));
    /* catan: atan(a / 4096) in 4096 per turn; its CORDIC is within a few units. */
    CHECK(catan(0) == 0, "catan(0) = %d", catan(0));
    worst = 0;
    for (a = -40000; a <= 40000; a += 13) {
        s32 d = catan(a) - (s32)lround(atan(a / 4096.0) * 2048.0 / PI);

        if (d < 0) {
            d = -d;
        }
        if (d > worst) {
            worst = d;
        }
    }
    CHECK(worst <= 4, "catan is %d from atan at worst", worst);
    CHECK(abs(catan(4096) - 512) <= 3, "catan(1.0) = %d", catan(4096));
    /* rsin/rcos (were there): a quarter turn. */
    CHECK(rsin(1024) == 4096 && rcos(0) == 4096 && rcos(2048) == -4096, "rsin/rcos");
}

static void test_matrices(void) {
    MATRIX m, n, m0, m1, m2, m3;
    SVECTOR r;
    VECTOR v, t;
    int i, j, k;
    s32 worst = 0;

    /* RotMatrix: Rx * Ry * Rz; identity at 0; against the product in doubles. */
    memset(&m, 0x55, sizeof(m));
    r.vx = r.vy = r.vz = 0;
    RotMatrix(&r, &m);
    CHECK(m.m[0][0] == 4096 && m.m[1][1] == 4096 && m.m[2][2] == 4096 && m.m[0][1] == 0 && m.m[2][0] == 0,
          "RotMatrix(0) is not the identity");
    CHECK(m.t[0] == 0x55555555, "RotMatrix wrote the translation");
    for (k = 0; k < 200; k++) {
        double ax = (k * 731 % 4096) * 2 * PI / 4096, ay = (k * 377 % 4096) * 2 * PI / 4096;
        double az = (k * 1999 % 4096) * 2 * PI / 4096;
        double cx = cos(ax), sx = sin(ax), cy = cos(ay), sy = sin(ay), cz = cos(az), sz = sin(az);
        /* Rx Ry Rz and Ry Rx Rz with Psy-Q's matrices (Rx = [1 0 0; 0 c -s; 0 s c], Ry = [c 0 s; 0 1 0; -s 0 c],
         * Rz = [c -s 0; s c 0; 0 0 1]). */
        double xyz[3][3] = { { cy * cz, -cy * sz, sy },
                             { sx * sy * cz + cx * sz, -sx * sy * sz + cx * cz, -sx * cy },
                             { -cx * sy * cz + sx * sz, cx * sy * sz + sx * cz, cx * cy } };
        double yxz[3][3] = { { cy * cz + sy * sx * sz, -cy * sz + sy * sx * cz, sy * cx },
                             { cx * sz, cx * cz, -sx },
                             { -sy * cz + cy * sx * sz, sy * sz + cy * sx * cz, cy * cx } };

        r.vx = (s16)(k * 731 % 4096);
        r.vy = (s16)(k * 377 % 4096);
        r.vz = (s16)(k * 1999 % 4096);
        RotMatrix(&r, &m);
        RotMatrixYXZ(&r, &n);
        for (i = 0; i < 3; i++) {
            for (j = 0; j < 3; j++) {
                s32 d1 = abs(m.m[i][j] - (s32)lround(xyz[i][j] * 4096));
                s32 d2 = abs(n.m[i][j] - (s32)lround(yxz[i][j] * 4096));

                worst = d1 > worst ? d1 : worst;
                worst = d2 > worst ? d2 : worst;
            }
        }
    }
    CHECK(worst <= 3, "RotMatrix/RotMatrixYXZ are %d from the exact product at worst", worst);
    /* A negative angle is the positive one's turn back. */
    r.vx = -100;
    r.vy = -2000;
    r.vz = -4000;
    RotMatrix(&r, &m);
    r.vx = 4096 - 100;
    r.vy = 4096 - 2000;
    r.vz = 4096 - 4000;
    RotMatrix(&r, &n);
    CHECK(memcmp(&m, &n, 18) == 0, "RotMatrix of negative angles differs from the same angles mod 4096");

    /* MulMatrix0 = MulMatrix's product into a third matrix; CompMatrix adds the translation. */
    r.vx = 300;
    r.vy = 1200;
    r.vz = -700;
    memset(&m0, 0, sizeof(m0));
    RotMatrix(&r, &m0);
    m0.t[0] = 100;
    m0.t[1] = -200;
    m0.t[2] = 3000;
    r.vx = -50;
    r.vy = 77;
    r.vz = 2048;
    memset(&m1, 0, sizeof(m1));
    RotMatrix(&r, &m1);
    m1.t[0] = 1000;
    m1.t[1] = 2000;
    m1.t[2] = -3000;
    memset(&m2, 0x77, sizeof(m2));
    CHECK(MulMatrix0(&m0, &m1, &m2) == &m2, "MulMatrix0's return");
    m3 = m0;
    MulMatrix(&m3, &m1);
    CHECK(memcmp(&m2, &m3, 20) == 0, "MulMatrix0 differs from MulMatrix");
    CHECK(m2.t[0] == 0x77777777, "MulMatrix0 wrote the translation");
    CHECK(CompMatrix(&m0, &m1, &m2) == &m2, "CompMatrix's return");
    CHECK(memcmp(&m2, &m3, 20) == 0, "CompMatrix's rotation differs from MulMatrix0's");
    for (i = 0; i < 3; i++) {
        s64 want = m0.t[i] + (((s64)m0.m[i][0] * m1.t[0] + (s64)m0.m[i][1] * m1.t[1] + (s64)m0.m[i][2] * m1.t[2]) >> 12);

        CHECK(m2.t[i] == want, "CompMatrix t[%d] = %d, want %lld", i, m2.t[i], (long long)want);
    }
    /* CompMatrix into one of its inputs. */
    m3 = m0;
    CompMatrix(&m3, &m1, &m3);
    CHECK(memcmp(&m3, &m2, sizeof(m2)) == 0, "CompMatrix(m0, m1, m0) differs");

    /* TransMatrix, the setters: the GTE's control registers. */
    v.vx = 11;
    v.vy = -22;
    v.vz = 33;
    CHECK(TransMatrix(&m0, &v) == &m0 && m0.t[0] == 11 && m0.t[1] == -22 && m0.t[2] == 33, "TransMatrix");
    SetRotMatrix(&m0);
    SetTransMatrix(&m0);
    SetLightMatrix(&m1);
    for (i = 0; i < 5; i++) {
        u32 w0, w1;

        memcpy(&w0, (u8 *)&m0 + 4 * i, 4);
        memcpy(&w1, (u8 *)&m1 + 4 * i, 4);
        if (i == 4) {
            w0 = (u32)(s32)(s16)w0;
            w1 = (u32)(s32)(s16)w1;
        }
        CHECK(psyq_gte_cfc2(i) == w0, "SetRotMatrix word %d", i);
        CHECK(psyq_gte_cfc2(8 + i) == w1, "SetLightMatrix word %d", i);
    }
    CHECK(psyq_gte_cfc2(5) == 11 && psyq_gte_cfc2(6) == (u32)-22 && psyq_gte_cfc2(7) == 33, "SetTransMatrix");

    /* VectorNormal: unit length in 4.12, the direction kept. */
    for (k = 1; k < 100; k++) {
        double len, d;

        v.vx = k * 97 % 3000 - 1500;
        v.vy = k * 53 % 2000 - 1000;
        v.vz = k * 31 % 1000 + 1;
        t.pad = 0x1234;
        VectorNormal(&v, &t);
        len = sqrt((double)t.vx * t.vx + (double)t.vy * t.vy + (double)t.vz * t.vz);
        d = sqrt((double)v.vx * v.vx + (double)v.vy * v.vy + (double)v.vz * v.vz);
        /* LIBGTE cuts |v|^2 to 8 bits before its 1 / sqrt table: up to 1/128 long, never short. */
        CHECK(len > 4096 * 0.999 && len < 4096 * 1.009, "VectorNormal(%d,%d,%d) has length %.1f", v.vx, v.vy, v.vz,
              len);
        CHECK(fabs(t.vx - v.vx / d * len) < 2 && fabs(t.vy - v.vy / d * len) < 2 && fabs(t.vz - v.vz / d * len) < 2,
              "VectorNormal's direction");
        CHECK(t.pad == 0x1234, "VectorNormal wrote the pad");
    }
    v.vx = v.vy = v.vz = 0;
    VectorNormal(&v, &t);
    CHECK(t.vx == 0 && t.vy == 0 && t.vz == 0, "VectorNormal(0) = %d,%d,%d", t.vx, t.vy, t.vz);

    /* MatrixNormal: rows of unit length, orthogonal; row 1 keeps its direction; RT unchanged. */
    memset(&m, 0, sizeof(m));
    m.m[0][0] = 3000;
    m.m[0][1] = 500;
    m.m[0][2] = 100;
    m.m[1][0] = 400;
    m.m[1][1] = 2900;
    m.m[1][2] = -300;
    m.m[2][0] = 1;
    SetRotMatrix(&m0);
    memset(&n, 0x66, sizeof(n));
    MatrixNormal(&m, &n);
    for (i = 0; i < 3; i++) {
        double len = sqrt((double)n.m[i][0] * n.m[i][0] + (double)n.m[i][1] * n.m[i][1] + (double)n.m[i][2] * n.m[i][2]);

        CHECK(len > 4096 * 0.999 && len < 4096 * 1.009, "MatrixNormal row %d has length %.1f", i, len);
        for (j = i + 1; j < 3; j++) {
            double dot = (double)n.m[i][0] * n.m[j][0] + (double)n.m[i][1] * n.m[j][1] + (double)n.m[i][2] * n.m[j][2];

            CHECK(fabs(dot) < 4096.0 * 4096 * 0.01, "MatrixNormal rows %d, %d not orthogonal (%.0f)", i, j, dot);
        }
    }
    CHECK(n.m[1][1] > 4000 && n.t[0] == 0x66666666, "MatrixNormal's row 1 / translation");
    {
        u32 w;

        memcpy(&w, &m0, 4);
        CHECK(psyq_gte_cfc2(0) == w, "MatrixNormal did not restore RT");
    }

    /* PushMatrix/PopMatrix: RT and TR round trip; 20 deep; a pop of an empty stack changes nothing. */
    {
        u32 before[8], after[8];
        int d;

        SetRotMatrix(&m0);
        SetTransMatrix(&m1);
        for (i = 0; i < 8; i++) {
            before[i] = psyq_gte_cfc2(i);
        }
        PushMatrix();
        SetRotMatrix(&m1);
        TransMatrix(&m3, &v);
        SetTransMatrix(&m3);
        PopMatrix();
        for (i = 0; i < 8; i++) {
            after[i] = psyq_gte_cfc2(i);
        }
        CHECK(memcmp(before, after, sizeof(before)) == 0, "PushMatrix/PopMatrix round trip");
        for (d = 0; d < 21; d++) { /* the 21st push is refused */
            psyq_gte_ctc2(5, (u32)d);
            PushMatrix();
        }
        for (d = 19; d >= 0; d--) {
            PopMatrix();
            CHECK(psyq_gte_cfc2(5) == (u32)d, "PopMatrix at depth %d: TRX %u", d, psyq_gte_cfc2(5));
        }
        psyq_gte_ctc2(5, 777);
        PopMatrix();
        CHECK(psyq_gte_cfc2(5) == 777, "PopMatrix of an empty stack changed TR");
    }
}

static void test_transforms(void) {
    SVECTOR v[4] = { { -100, -50, 0, 0 }, { 120, -40, 30, 0 }, { 10, 90, -20, 0 }, { 300, 200, 100, 0 } };
    s32 sxy[4], s1[4], p, p1, flag, flag1, otz, otz4, ret, ret1, f[4], pz[4];
    VECTOR out;
    int i;

    setup_camera(200);
    /* RotTransPers3 = three RotTransPers: the screen points; p, otz of the last; FLAG the three's. */
    for (i = 0; i < 3; i++) {
        ret = RotTransPers(&v[i], &s1[i], &pz[i], &f[i]);
    }
    ret1 = RotTransPers3(&v[0], &v[1], &v[2], &sxy[0], &sxy[1], &sxy[2], &p, &flag);
    for (i = 0; i < 3; i++) {
        CHECK(sxy[i] == s1[i], "RotTransPers3 vertex %d: %08x vs RotTransPers %08x", i, sxy[i], s1[i]);
    }
    CHECK(ret1 == ret && p == pz[2], "RotTransPers3's otz %d / p %d vs %d / %d", ret1, p, ret, pz[2]);
    CHECK(flag == (f[0] | f[1] | f[2]), "RotTransPers3's flag %08x vs %08x", flag, f[0] | f[1] | f[2]);
    CHECK((flag & 0x80000000u) == 0, "a camera in view sets FLAG's error bit: %08x", flag);
    CHECK(ret == (s32)(psyq_gte_mfc2(19) >> 2), "the otz is SZ3 / 4");
    /* RotTransPers4 = RotTransPers3 + RotTransPers; RotAverage4 returns the average depth. */
    ret = RotTransPers(&v[3], &s1[3], &pz[3], &f[3]);
    ret1 = RotTransPers4(&v[0], &v[1], &v[2], &v[3], &sxy[0], &sxy[1], &sxy[2], &sxy[3], &p, &flag);
    CHECK(memcmp(sxy, s1, sizeof(sxy)) == 0 && ret1 == ret && p == pz[3], "RotTransPers4");
    otz = RotAverage4(&v[0], &v[1], &v[2], &v[3], &sxy[0], &sxy[1], &sxy[2], &sxy[3], &p, &flag);
    otz4 = otz;
    {
        s32 sum = 0;

        for (i = 16; i < 20; i++) {
            sum += (s32)psyq_gte_mfc2(i);
        }
        CHECK(abs(otz - sum / 16) <= 1, "RotAverage4 = %d, the SZs' sum / 16 = %d", otz, sum / 16);
    }
    /* RotNclip3/4, RotAverageNclip3/4: the outer product's sign; nothing but the flag stored for a back face. */
    {
        s32 opz = RotNclip3(&v[0], &v[1], &v[2], &sxy[0], &sxy[1], &sxy[2], &p, &otz, &flag);
        s32 n0 = (s32)(s16)s1[0], n1 = (s32)(s16)s1[1], n2 = (s32)(s16)s1[2];
        s32 m0 = s1[0] >> 16, m1 = s1[1] >> 16, m2 = s1[2] >> 16;
        s32 want = n0 * m1 + n1 * m2 + n2 * m0 - n0 * m2 - n1 * m0 - n2 * m1;
        s32 sentinel = 0x5A5A5A5A;
        s32 back[3] = { sentinel, sentinel, sentinel }, bp = sentinel, botz = sentinel, bflag = sentinel;

        CHECK(opz == want, "RotNclip3 returns %d, the outer product is %d", opz, want);
        if (opz > 0) {
            CHECK(sxy[0] == s1[0] && otz == RotTransPers(&v[2], &s1[2], &pz[2], &f[2]), "RotNclip3's stores");
            ret = RotNclip3(&v[0], &v[2], &v[1], &back[0], &back[1], &back[2], &bp, &botz, &bflag);
        } else {
            ret = RotNclip3(&v[0], &v[1], &v[2], &back[0], &back[1], &back[2], &bp, &botz, &bflag);
        }
        CHECK(ret <= 0 && back[0] == sentinel && back[2] == sentinel && bp == sentinel && botz == sentinel &&
              bflag != sentinel, "RotNclip3 of a back face stored more than the flag");
        if (opz > 0) {
            s32 o3 = 0, o4 = 0;

            CHECK(RotAverageNclip3(&v[0], &v[1], &v[2], &sxy[0], &sxy[1], &sxy[2], &p, &o3, &flag) == opz,
                  "RotAverageNclip3's return");
            CHECK(abs(o3 - (s32)(psyq_gte_mfc2(17) + psyq_gte_mfc2(18) + psyq_gte_mfc2(19)) / 12) <= 1,
                  "RotAverageNclip3's otz %d", o3);
            CHECK(RotAverageNclip4(&v[0], &v[1], &v[2], &v[3], &sxy[0], &sxy[1], &sxy[2], &sxy[3], &p, &o4, &flag) ==
                      opz, "RotAverageNclip4's return");
            CHECK(sxy[3] == s1[3] && o4 == otz4, "RotAverageNclip4 %08x / %d, RotAverage4's %08x / %d", sxy[3], o4,
                  s1[3], otz4);
            /* RotNclip4 returns the flag word for a front face (LIBGTE reuses its return register). */
            ret = RotNclip4(&v[0], &v[1], &v[2], &v[3], &sxy[0], &sxy[1], &sxy[2], &sxy[3], &p, &otz, &flag);
            CHECK(ret == flag && sxy[3] == s1[3] && otz == (s32)(psyq_gte_mfc2(19) >> 2), "RotNclip4");
        } else {
            CHECK(0, "the test triangle should face the camera (outer product %d)", opz);
        }
    }
    /* RotTrans: RT * v + TR, no perspective. */
    {
        MATRIX id;
        SVECTOR u = { 100, -200, 300, 0 };

        memset(&id, 0, sizeof(id));
        id.m[0][0] = id.m[1][1] = id.m[2][2] = 4096;
        id.t[0] = 5;
        id.t[1] = 6;
        id.t[2] = 7;
        SetRotMatrix(&id);
        SetTransMatrix(&id);
        out.pad = 99;
        RotTrans(&u, &out, &flag1);
        CHECK(out.vx == 105 && out.vy == -194 && out.vz == 307 && flag1 == 0 && out.pad == 99, "RotTrans %d,%d,%d",
              out.vx, out.vy, out.vz);
    }
    /* NormalColorCol(3): no light (LLM = 0), the back colour half: the colour halved. */
    {
        MATRIX zero;
        SVECTOR nrm = { 0, 0, 4096, 0 };
        CVECTOR in = { 200, 100, 50, 0x30 }, o[3];

        memset(&zero, 0, sizeof(zero));
        SetLightMatrix(&zero);
        SetColorMatrix(&zero);
        SetBackColor(128, 128, 128);
        NormalColorCol(&nrm, &in, &o[0]);
        CHECK(abs(o[0].r - 100) <= 1 && abs(o[0].g - 50) <= 1 && abs(o[0].b - 25) <= 1 && o[0].cd == 0x30,
              "NormalColorCol = %d,%d,%d,%02x", o[0].r, o[0].g, o[0].b, o[0].cd);
        NormalColorCol3(&nrm, &nrm, &nrm, &in, &o[0], &o[1], &o[2]);
        CHECK(memcmp(&o[0], &o[1], 4) == 0 && memcmp(&o[1], &o[2], 4) == 0 && abs(o[2].r - 100) <= 1,
              "NormalColorCol3");
    }
    (void)p1;
}

/* ---- LIBGPU ---- */

static void test_packets(void) {
    static const struct {
        const char *name;
        void (*set)(void *);
        u8 len, code;
    } prims[] = {
        { "SetPolyF3", (void (*)(void *))SetPolyF3, 4, 0x20 },   { "SetPolyFT3", (void (*)(void *))SetPolyFT3, 7, 0x24 },
        { "SetPolyG3", (void (*)(void *))SetPolyG3, 6, 0x30 },   { "SetPolyGT3", (void (*)(void *))SetPolyGT3, 9, 0x34 },
        { "SetPolyF4", (void (*)(void *))SetPolyF4, 5, 0x28 },   { "SetPolyFT4", (void (*)(void *))SetPolyFT4, 9, 0x2C },
        { "SetPolyG4", (void (*)(void *))SetPolyG4, 8, 0x38 },   { "SetPolyGT4", (void (*)(void *))SetPolyGT4, 12, 0x3C },
        { "SetLineF2", (void (*)(void *))SetLineF2, 3, 0x40 },   { "SetLineG2", (void (*)(void *))SetLineG2, 4, 0x50 },
        { "SetLineG3", (void (*)(void *))SetLineG3, 7, 0x58 },   { "SetTile", (void (*)(void *))SetTile, 3, 0x60 },
        { "SetLineF3", (void (*)(void *))SetLineF3, 5, 0x48 },   { "SetLineF4", (void (*)(void *))SetLineF4, 6, 0x4C },
        { "SetLineG4", (void (*)(void *))SetLineG4, 9, 0x5C },   { "SetSprt8", (void (*)(void *))SetSprt8, 3, 0x74 },
        { "SetSprt16", (void (*)(void *))SetSprt16, 3, 0x7C },   { "SetTile1", (void (*)(void *))SetTile1, 2, 0x68 },
        { "SetTile8", (void (*)(void *))SetTile8, 2, 0x70 },     { "SetTile16", (void (*)(void *))SetTile16, 2, 0x78 },
    };
    u8 buf[64], ref[64];
    size_t i;

    for (i = 0; i < sizeof(prims) / sizeof(prims[0]); i++) {
        memset(buf, 0xAA, sizeof(buf));
        memcpy(ref, buf, sizeof(buf));
        prims[i].set(buf);
        ref[3] = prims[i].len;
        ref[7] = prims[i].code;
        if (prims[i].code == 0x58) { /* LINE_G3's terminator */
            memset(ref + 28, 0x55, 4);
        }
        if (prims[i].code == 0x48) { /* LINE_F3's */
            memset(ref + 20, 0x55, 4);
        }
        if (prims[i].code == 0x4C) { /* LINE_F4's */
            memset(ref + 24, 0x55, 4);
        }
        if (prims[i].code == 0x5C) { /* LINE_G4's */
            memset(ref + 36, 0x55, 4);
        }
        CHECK(memcmp(buf, ref, sizeof(buf)) == 0, "%s: len %u code %02x", prims[i].name, buf[3], buf[7]);
    }
    /* SetPolyGT4 on a whole primitive: only the length byte and the command byte change. */
    {
        POLY_GT4 g, h;

        memset(&g, 0x11, sizeof(g));
        h = g;
        SetPolyGT4(&g);
        ((u8 *)&h)[3] = 12;
        h.code = 0x3C;
        CHECK(sizeof(POLY_GT4) == 52 && memcmp(&g, &h, sizeof(g)) == 0, "SetPolyGT4's bytes");
    }
    CHECK(sizeof(LINE_F3) == 24 && sizeof(LINE_F4) == 28 && sizeof(LINE_G4) == 40 && sizeof(SPRT_8) == 16 &&
              sizeof(SPRT_16) == 16 && sizeof(TILE_1) == 12 && sizeof(TILE_8) == 12 && sizeof(TILE_16) == 12,
          "the new forms' sizes");
    /* SetSemiTrans/SetShadeTex: bits 1 and 0 of the command byte. */
    {
        POLY_FT4 q;

        memset(&q, 0, sizeof(q));
        SetPolyFT4(&q);
        SetShadeTex(&q, 1);
        SetSemiTrans(&q, 1);
        CHECK(q.code == 0x2F, "SetShadeTex/SetSemiTrans on: %02x", q.code);
        SetShadeTex(&q, 0);
        CHECK(q.code == 0x2E, "SetShadeTex off: %02x", q.code);
    }
    /* The drawing-mode packets. */
    {
        DR_AREA a;
        DR_TWIN w;
        DR_STP s;
        DR_MODE m;
        RECT r = { 16, 32, 64, 32 };
        RECT big = { -5, 100, 2000, 600 };

        SetDrawArea(&a, &r);
        CHECK((a.tag >> 24) == 2 && a.code[0] == (0xE3000000u | (32 << 10) | 16) &&
              a.code[1] == (0xE4000000u | (63 << 10) | 79), "SetDrawArea %08x %08x", a.code[0], a.code[1]);
        SetDrawArea(&a, &big);
        CHECK(a.code[0] == (0xE3000000u | (100 << 10)) && a.code[1] == (0xE4000000u | (511 << 10) | 1023),
              "SetDrawArea clamps: %08x %08x", a.code[0], a.code[1]);
        SetTexWindow(&w, &r);
        CHECK((w.tag >> 24) == 2 && w.code[0] == (0xE2000000u | (4 << 15) | (2 << 10) | (28 << 5) | 24) &&
              w.code[1] == 0, "SetTexWindow %08x", w.code[0]);
        SetTexWindow(&w, NULL);
        CHECK(w.code[0] == 0 && w.code[1] == 0, "SetTexWindow(NULL)");
        SetDrawStp(&s, 1);
        CHECK((s.tag >> 24) == 2 && s.code[0] == 0xE6000001u && s.code[1] == 0, "SetDrawStp(1)");
        SetDrawStp(&s, 0);
        CHECK(s.code[0] == 0xE6000000u, "SetDrawStp(0)");
        SetDrawMode(&m, 1, 1, 0x1A5, &r);
        CHECK((m.tag >> 24) == 2 && m.code[0] == (0xE1000000u | 0x600 | 0x1A5) && m.code[1] == (0xE2000000u | (4 << 15) | (2 << 10) | (28 << 5) | 24),
              "SetDrawMode %08x %08x", m.code[0], m.code[1]);
        SetDrawMode(&m, 0, 0, 0xFFFF, NULL);
        CHECK(m.code[0] == (0xE1000000u | 0x9FF) && m.code[1] == 0, "SetDrawMode without a window");
    }
    /* MargePrim: p1 joins p0's packet. */
    {
        u32 *p0 = &arena[100], *p1 = &arena[103];

        p0[0] = 0x02FFFFFF;
        p1[0] = 0x04123456;
        CHECK(MargePrim(p0, p1) == 0 && (p0[0] >> 24) == 7 && (p0[0] & 0xFFFFFF) == 0xFFFFFF && p1[0] == 0,
              "MargePrim: %08x %08x", p0[0], p1[0]);
        p0[0] = 0x0CFFFFFF;
        p1[0] = 0x04123456;
        CHECK(MargePrim(p0, p1) == -1 && p0[0] == 0x0CFFFFFF && p1[0] == 0x04123456, "MargePrim past 16 words");
    }
}

/* AddPrim links a primitive after an ordering-table entry, and DrawOTag draws it. */
static void test_addprim(void) {
    u32 *ot = &arena[1000];
    TILE *t = (TILE *)&arena[2000];
    TILE *t2 = (TILE *)&arena[2010];
    const u16 *vram = psyq_gpu_vram();

    ClearOTag(ot, 4);
    SetTile(t);
    setRGB0(t, 255, 0, 0);
    t->x0 = 10;
    t->y0 = 20;
    t->w = 4;
    t->h = 3;
    SetTile(t2);
    setRGB0(t2, 0, 0, 255);
    t2->x0 = 12;
    t2->y0 = 20;
    t2->w = 1;
    t2->h = 1;
    AddPrim(&ot[2], t);
    CHECK((ot[2] & 0xFFFFFF) == port_ptr_to_u32(t), "AddPrim: ot's link %06x", ot[2] & 0xFFFFFF);
    CHECK((t->tag & 0xFFFFFF) == port_ptr_to_u32(&ot[3]) && (t->tag >> 24) == 3, "AddPrim: the primitive's tag %08x",
          t->tag);
    AddPrim(&ot[2], t2); /* inserted before t: drawn first, so t covers it */
    CHECK((ot[2] & 0xFFFFFF) == port_ptr_to_u32(t2) && (t2->tag & 0xFFFFFF) == port_ptr_to_u32(t), "AddPrim twice");
    {
        DRAWENV env;
        DR_ENV *de = (DR_ENV *)&arena[3000];

        SetDefDrawEnv(&env, 0, 0, 320, 240);
        SetDrawEnv(de, &env);
        termPrim(de);
        DrawOTag((u32 *)de);
    }
    DrawOTag(ot);
    CHECK(vram[20 * 1024 + 10] == 0x001F && vram[22 * 1024 + 13] == 0x001F && vram[20 * 1024 + 14] == 0,
          "DrawOTag after AddPrim: %04x %04x %04x", vram[20 * 1024 + 10], vram[22 * 1024 + 13], vram[20 * 1024 + 14]);
    CHECK(vram[20 * 1024 + 12] == 0x001F, "the primitive added last is drawn first");
}

static void test_images(void) {
    static u16 pix[37 * 5 + 1], back[37 * 5 + 3];
    RECT r = { 1010, 509, 37, 5 }; /* wraps at both edges */
    RECT empty = { 0, 0, 0, 5 };
    const u16 *vram = psyq_gpu_vram();
    int i;

    for (i = 0; i < 37 * 5; i++) {
        pix[i] = (u16)(i * 977 + 13);
    }
    CHECK(LoadImage2(&r, (u32 *)pix) == 0, "LoadImage2's return");
    CHECK(vram[509 * 1024 + 1010] == pix[0] && vram[511 * 1024 + 1023] == pix[2 * 37 + 13] &&
          vram[0 * 1024 + 0] == pix[3 * 37 + 14], "LoadImage2 wrapped");
    memset(back, 0xEE, sizeof(back));
    CHECK(StoreImage(&r, (u32 *)back) == 0, "StoreImage's return");
    CHECK(memcmp(back, pix, 37 * 5 * 2) == 0, "StoreImage of a LoadImage differs");
    CHECK(back[37 * 5] == 0 && back[37 * 5 + 1] == 0xEEEE, "StoreImage's last word: %04x %04x", back[37 * 5],
          back[37 * 5 + 1]);
    memset(back, 0xEE, sizeof(back));
    CHECK(StoreImage2(&r, (u32 *)back) == 0 && memcmp(back, pix, 37 * 5 * 2) == 0, "StoreImage2");
    StoreImage(&empty, (u32 *)back);
    CHECK(back[0] == pix[0], "StoreImage of an empty rectangle wrote");
    {
        RECT src = { 1010, 509, 8, 2 };

        CHECK(MoveImage2(&src, 100, 100) == 0, "MoveImage2's return");
        CHECK(vram[100 * 1024 + 100] == pix[0] && vram[101 * 1024 + 107] == pix[37 + 7], "MoveImage2 copied");
        CHECK(MoveImage2(&empty, 0, 0) == -1, "MoveImage2 of an empty rectangle");
    }
}

static void test_envs(void) {
    DISPENV d, g;
    DRAWENV e;
    const u16 *vram = psyq_gpu_vram();
    u8 ff[sizeof(DISPENV)];

    ResetGraph(0);
    memset(ff, 0xFF, sizeof(ff));
    GetDispEnv(&g);
    CHECK(memcmp(&g, ff, sizeof(g)) == 0, "GetDispEnv after ResetGraph(0) is not all 0xFF");
    SetDefDispEnv(&d, 0, 256, 320, 240);
    d.isinter = 0;
    PutDispEnv(&d);
    memset(&g, 0, sizeof(g));
    CHECK(GetDispEnv(&g) == &g && memcmp(&g, &d, sizeof(d)) == 0, "GetDispEnv after PutDispEnv");
    SetDispMask(1);
    GetDispEnv(&g);
    CHECK(memcmp(&g, &d, sizeof(d)) == 0, "SetDispMask(1) changed GetDispEnv's");
    SetDispMask(0);
    GetDispEnv(&g);
    CHECK(memcmp(&g, ff, sizeof(g)) == 0, "GetDispEnv after SetDispMask(0)");

    /* PutDrawEnv: the environment applied and its background drawn now; a 64-aligned clip is cleared with a fill
     * (not offset), another with a tile relative to the offset; the packet is terminated. */
    SetDefDrawEnv(&e, 64, 256, 128, 16);
    e.isbg = 1;
    setRGB0(&e, 0, 255, 0);
    CHECK(PutDrawEnv(&e) == &e, "PutDrawEnv's return");
    CHECK((e.dr_env.tag & 0xFFFFFF) == 0xFFFFFF && (e.dr_env.tag >> 24) == 9, "PutDrawEnv's packet tag %08x",
          e.dr_env.tag);
    CHECK(e.dr_env.code[6] == 0x0200FF00u && e.dr_env.code[7] == ((256u << 16) | 64), "PutDrawEnv's fill %08x %08x",
          e.dr_env.code[6], e.dr_env.code[7]);
    CHECK(vram[256 * 1024 + 64] == 0x03E0 && vram[271 * 1024 + 191] == 0x03E0 && vram[272 * 1024 + 64] != 0x03E0,
          "PutDrawEnv's background");
    SetDefDrawEnv(&e, 10, 300, 20, 4);
    e.isbg = 1;
    setRGB0(&e, 255, 255, 255);
    PutDrawEnv(&e);
    CHECK(e.dr_env.code[6] == 0x60FFFFFFu && e.dr_env.code[7] == 0, "PutDrawEnv's tile %08x %08x",
          e.dr_env.code[6], e.dr_env.code[7]);
    CHECK(vram[300 * 1024 + 10] == 0x7FFF && vram[303 * 1024 + 29] == 0x7FFF && vram[300 * 1024 + 30] != 0x7FFF,
          "PutDrawEnv's tile drawn");
    {
        DR_ENV a;

        memset(&a, 0, sizeof(a));
        e.isbg = 0;
        SetDrawEnv(&a, &e);
        PutDrawEnv(&e);
        CHECK(memcmp(a.code, e.dr_env.code, 6 * 4) == 0 && (a.tag >> 24) == (e.dr_env.tag >> 24),
              "PutDrawEnv's packet differs from SetDrawEnv's");
    }
}

/* Two TIMs back to back (one with a CLUT), then something else. */
static void test_tim(void) {
    u32 *tim = &arena[5000];
    u32 n = 0, first_img, second;
    TIM_IMAGE ti;

    tim[n++] = 0x10;
    tim[n++] = 0x08 | 0; /* 4-bit, CLUT */
    tim[n++] = 12 + 32;  /* the CLUT block: 16 colours */
    tim[n++] = (480u << 16) | 768;
    tim[n++] = (1u << 16) | 16;
    n += 8;
    first_img = n;
    tim[n++] = 12 + 8;  /* the image block: 4 words of pixels */
    tim[n++] = (0u << 16) | 640;
    tim[n++] = (2u << 16) | 2;
    n += 2;
    second = n;
    tim[n++] = 0x10;
    tim[n++] = 0x02; /* 15-bit, no CLUT */
    tim[n++] = 12 + 4;
    tim[n++] = (10u << 16) | 20;
    tim[n++] = (1u << 16) | 2;
    n += 1;
    tim[n++] = 0x12345678;
    CHECK(OpenTIM(tim) == 0, "OpenTIM's return");
    CHECK(ReadTIM(&ti) == &ti, "ReadTIM of the first TIM");
    CHECK(ti.mode == 8 && ti.crect == (RECT *)&tim[3] && ti.caddr == &tim[5] && ti.prect == (RECT *)&tim[first_img + 1] &&
          ti.paddr == &tim[first_img + 3] && ti.crect->x == 768 && ti.prect->w == 2, "ReadTIM's first image");
    CHECK(ReadTIM(&ti) == &ti && ti.mode == 2 && ti.crect == NULL && ti.caddr == NULL &&
          ti.prect == (RECT *)&tim[second + 3] && ti.paddr == &tim[second + 5], "ReadTIM's second image");
    CHECK(ReadTIM(&ti) == NULL, "ReadTIM past the last TIM");
}


/* ---- LIBGS ---- */

static const MATRIX gs_identity = { { { 4096, 0, 0 }, { 0, 4096, 0 }, { 0, 0, 4096 } }, { 0, 0, 0 } };

static int gs_matrix_is(const MATRIX *m, const s16 r[9], s32 tx, s32 ty, s32 tz) {
    int i;

    for (i = 0; i < 9; i++) {
        if (m->m[i / 3][i % 3] != r[i]) {
            return 0;
        }
    }
    return m->t[0] == tx && m->t[1] == ty && m->t[2] == tz;
}

static void gs_print(const char *what, const MATRIX *m) {
    printf("  %s: %d %d %d / %d %d %d / %d %d %d t %d %d %d\n", what, m->m[0][0], m->m[0][1], m->m[0][2], m->m[1][0],
           m->m[1][1], m->m[1][2], m->m[2][0], m->m[2][1], m->m[2][2], (int)m->t[0], (int)m->t[1], (int)m->t[2]);
}

/* GsSetRefView2 against views worked out by hand (GsInitGraph(320, 240): the aspect is 1, the base the identity). The
 * distances are powers of two, where LIBGTE's SquareRoot0 (a table: sqrt(1000 * 1000) is 999) is exact. */
static void test_gs_view(void) {
    static const struct {
        s32 vp[3], vr[3], rz;
        s16 m[9];
        s32 t[3];
    } views[] = {
        /* along +z from 1024 behind the origin: the identity, the origin 1024 ahead */
        { { 0, 0, -1024 }, { 0, 0, 0 }, 0, { 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 }, { 0, 0, 1024 } },
        /* from +x towards the origin: a quarter turn about y (sin 4096, cos 0) */
        { { 1024, 0, 0 }, { 0, 0, 0 }, 0, { 0, 0, 4096, 0, 4096, 0, -4096, 0, 0 }, { 0, 0, 1024 } },
        /* from above (-y: the PS1's y is down) straight down: a quarter turn about x, no y turn (rxz = 0) */
        { { 0, -1024, 0 }, { 0, 0, 0 }, 0, { 4096, 0, 0, 0, 0, -4096, 0, 4096, 0 }, { 0, 0, 1024 } },
        /* along +z with a 90 degree twist (rz = 90 << 12): Rz(-90) */
        { { 0, 0, -1024 }, { 0, 0, 0 }, 90 << 12, { 0, 4096, 0, -4096, 0, 0, 0, 0, 4096 }, { 0, 0, 1024 } },
        /* translated: from (100, 200, -724) along +z */
        { { 100, 200, -724 }, { 100, 200, 300 }, 0, { 4096, 0, 0, 0, 4096, 0, 0, 0, 4096 }, { -100, -200, 724 } },
    };
    size_t i;

    psyq_gs_reset();
    GsInitGraph(320, 240, 0, 0, 0);
    for (i = 0; i < sizeof(views) / sizeof(views[0]); i++) {
        GsRVIEW2 v;
        s32 r;

        memset(&v, 0, sizeof(v));
        v.vpx = views[i].vp[0];
        v.vpy = views[i].vp[1];
        v.vpz = views[i].vp[2];
        v.vrx = views[i].vr[0];
        v.vry = views[i].vr[1];
        v.vrz = views[i].vr[2];
        v.rz = views[i].rz;
        r = GsSetRefView2(&v);
        CHECK(r == 0 && gs_matrix_is(&GsWSMATRIX, views[i].m, views[i].t[0], views[i].t[1], views[i].t[2]),
              "GsSetRefView2 view %d: returned %d", (int)i, r);
        if (!gs_matrix_is(&GsWSMATRIX, views[i].m, views[i].t[0], views[i].t[1], views[i].t[2])) {
            gs_print("GsWSMATRIX", &GsWSMATRIX);
        }
    }
}

/* GsGetLw/GsGetLs/GsGetLws through a root, a middle and a leaf system; flg's cache; GsInitCoordinate2. */
static void test_gs_coords(void) {
    static GsCOORDINATE2 root, mid, leaf;
    static const s16 rz90[9] = { 0, -4096, 0, 4096, 0, 0, 0, 0, 4096 };
    MATRIX lw, ls, m;
    GsRVIEW2 v;

    psyq_gs_reset();
    GsInitGraph(320, 240, 0, 0, 0);
    memset(&v, 0, sizeof(v));
    v.vpz = -1024;
    GsSetRefView2(&v); /* the identity, t (0, 0, 1024) */
    memset(&root, 0x55, sizeof(root));
    memset(&mid, 0x55, sizeof(mid));
    memset(&leaf, 0x55, sizeof(leaf));
    GsInitCoordinate2(NULL, &root);
    GsInitCoordinate2(&root, &mid);
    GsInitCoordinate2(&mid, &leaf);
    CHECK(memcmp(&root.coord, &gs_identity, sizeof(MATRIX)) == 0 && root.flg == 0 && root.super == NULL,
          "GsInitCoordinate2: the identity, flg 0, no parent");
    CHECK(mid.super == &root && root.sub == &mid && leaf.super == &mid && mid.sub == &leaf,
          "GsInitCoordinate2: the parent and its sub");
    root.coord.t[0] = 100;
    mid.coord.m[0][0] = 0;
    mid.coord.m[0][1] = -4096;
    mid.coord.m[1][0] = 4096;
    mid.coord.m[1][1] = 0;
    mid.coord.t[1] = 50;
    leaf.coord.t[0] = 10;
    /* lw = root * mid * leaf: Rz(90), t = root.t + mid.t + Rz(90) * leaf.t = (100, 60, 0); ls adds the view's z */
    GsGetLws(&leaf, &lw, &ls);
    CHECK(gs_matrix_is(&lw, rz90, 100, 60, 0), "GsGetLws: the local-world matrix");
    CHECK(gs_matrix_is(&ls, rz90, 100, 60, 1024), "GsGetLws: the local-screen matrix");
    if (!gs_matrix_is(&lw, rz90, 100, 60, 0) || !gs_matrix_is(&ls, rz90, 100, 60, 1024)) {
        gs_print("lw", &lw);
        gs_print("ls", &ls);
    }
    CHECK(root.flg == 1 && mid.flg == 1 && leaf.flg == 1 && gs_matrix_is(&leaf.workm, rz90, 100, 60, 0) &&
          gs_matrix_is(&mid.workm, rz90, 100, 50, 0), "GsGetLws: each workm and flg = PSDCNT");
    GsGetLs(&leaf, &m);
    CHECK(memcmp(&m, &ls, sizeof(m)) == 0, "GsGetLs differs from GsGetLws's ls");
    GsGetLw(&leaf, &m);
    CHECK(memcmp(&m, &lw, sizeof(m)) == 0, "GsGetLw differs from GsGetLws's lw");
    /* the cache: a change without flg 0 is not seen; flg 0 on the leaf recomputes from the middle's cached workm */
    mid.coord.t[1] = 70;
    GsGetLw(&leaf, &m);
    CHECK(gs_matrix_is(&m, rz90, 100, 60, 0), "GsGetLw used the cache");
    leaf.coord.t[0] = 20;
    leaf.flg = 0;
    GsGetLw(&leaf, &m);
    CHECK(gs_matrix_is(&m, rz90, 100, 70, 0), "GsGetLw after the leaf's flg 0: t %d %d %d", (int)m.t[0],
          (int)m.t[1], (int)m.t[2]);
    mid.flg = 0; /* not seen while the leaf's own workm is current */
    GsGetLw(&leaf, &m);
    CHECK(gs_matrix_is(&m, rz90, 100, 70, 0), "GsGetLw with the middle's flg 0 alone: t %d %d %d", (int)m.t[0],
          (int)m.t[1], (int)m.t[2]);
    mid.flg = 0;
    leaf.flg = 0;
    GsGetLw(&leaf, &m);
    CHECK(gs_matrix_is(&m, rz90, 100, 90, 0), "GsGetLw after the middle's and the leaf's flg 0: t %d %d %d",
          (int)m.t[0], (int)m.t[1], (int)m.t[2]);
    /* GsMulCoord3: m1 = m1 * m2, the translation too */
    m = gs_identity;
    m.m[0][0] = 0;
    m.m[0][1] = -4096;
    m.m[1][0] = 4096;
    m.m[1][1] = 0;
    m.t[2] = 7;
    lw = gs_identity;
    lw.t[0] = 3;
    GsMulCoord3(&m, &lw);
    CHECK(gs_matrix_is(&m, rz90, 0, 3, 7), "GsMulCoord3: t %d %d %d", (int)m.t[0], (int)m.t[1], (int)m.t[2]);
}

/* GsSetLsMatrix, GsSetLightMatrix, GsSetAmbient, GsSetLightMode, the work base, GsSwapDispBuff. */
static void test_gs_setup(void) {
    MATRIX m = gs_identity, lt;
    DISPENV d;
    u8 buf[16];

    psyq_gs_reset();
    GsInitGraph(320, 240, 0, 0, 0);
    GsInit3D();
    CHECK(psyq_gte_cfc2(24) == 160u << 16 && psyq_gte_cfc2(25) == 120u << 16, "GsInit3D: the GTE's offset %08x %08x",
          psyq_gte_cfc2(24), psyq_gte_cfc2(25));
    m.m[0][1] = 123;
    m.t[0] = -5;
    m.t[2] = 900;
    GsSetLsMatrix(&m);
    CHECK(psyq_gte_cfc2(0) == ((123u << 16) | 4096) && psyq_gte_cfc2(5) == (u32)-5 && psyq_gte_cfc2(7) == 900,
          "GsSetLsMatrix: RT and TR");
    /* the light matrix is GsLIGHTWSMATRIX * m; the GTE's rotation is kept */
    GsLIGHTWSMATRIX = gs_identity;
    GsLIGHTWSMATRIX.m[0][0] = 2048;
    lt = gs_identity;
    lt.m[0][1] = 4096;
    GsSetLightMatrix(&lt);
    CHECK(psyq_gte_cfc2(8) == ((2048u << 16) | 2048) && psyq_gte_cfc2(0) == ((123u << 16) | 4096),
          "GsSetLightMatrix: L11/L12 %08x, RT %08x", psyq_gte_cfc2(8), psyq_gte_cfc2(0));
    GsSetAmbient(0x80, 0x40, 0x1F);
    CHECK(psyq_gte_cfc2(13) == 0x80 && psyq_gte_cfc2(14) == 0x40 && psyq_gte_cfc2(15) == 0x10,
          "GsSetAmbient: RBK %x %x %x", psyq_gte_cfc2(13), psyq_gte_cfc2(14), psyq_gte_cfc2(15));
    GsSetLightMode(3);
    GsSetLightMode(7);
    CHECK(psyq_gs.light_mode == 3, "GsSetLightMode(7) is ignored: %d", psyq_gs.light_mode);
    GsSetLightMode(0);
    GsSetWorkBase(buf);
    CHECK(GsGetWorkBase() == buf && GsOUT_PACKET_P == buf, "GsSetWorkBase/GsGetWorkBase");

    /* GsSwapDispBuff: the display shown, PSDCNT counted (0 skipped), the buffers alternate, the offset put again */
    CHECK(D_800812D8 == 1 && psyq_gs.idx == 0, "after GsInitGraph: PSDCNT %u buffer %d", D_800812D8, psyq_gs.idx);
    SetGeomOffset(0, 0);
    GsSwapDispBuff();
    GetDispEnv(&d);
    CHECK(D_800812D8 == 2 && psyq_gs.idx == 1, "GsSwapDispBuff once: PSDCNT %u buffer %d", D_800812D8, psyq_gs.idx);
    CHECK(d.disp.x == 0 && d.disp.y == 0 && d.disp.w == 320 && d.disp.h == 240, "GsSwapDispBuff's display %d,%d %dx%d",
          d.disp.x, d.disp.y, d.disp.w, d.disp.h);
    CHECK(psyq_gte_cfc2(24) == 160u << 16 && psyq_gte_cfc2(25) == 120u << 16, "GsSwapDispBuff: the GTE's offset");
    CHECK(psyq_gs.draw.clip.w == 320 && psyq_gs.draw.clip.h == 240, "GsSwapDispBuff: the drawing clip");
    GsSwapDispBuff();
    CHECK(D_800812D8 == 3 && psyq_gs.idx == 0, "GsSwapDispBuff twice: PSDCNT %u buffer %d", D_800812D8, psyq_gs.idx);
    GsSwapDispBuff();
    CHECK(psyq_gs.idx == 1, "GsSwapDispBuff three times: buffer %d", psyq_gs.idx);
    D_800812D8 = 0xFFFFFFFFu;
    GsSwapDispBuff();
    CHECK(D_800812D8 == 1 && psyq_gs.idx == 0, "GsSwapDispBuff: PSDCNT wraps to 1, not 0 (%u)", D_800812D8);
    /* GsOFSGPU (intl bit 2): the offset goes to the drawing environment, the GTE's is 0 */
    GsInitGraph(320, 240, 4, 0, 0);
    GsInit3D();
    CHECK(psyq_gs.draw.ofs[0] == 160 && psyq_gs.draw.ofs[1] == 120 && psyq_gte_cfc2(24) == 0,
          "GsInit3D with GsOFSGPU: the drawing offset %d,%d", psyq_gs.draw.ofs[0], psyq_gs.draw.ofs[1]);
}

/* A synthetic TMD (no game data): two objects. Object 0: four vertices, one normal, a run of two F3 (the first front-
 * facing, the second wound the other way) and one G4; object 1: one vertex and one NF4-less F3 (only its table entry
 * matters). Returns the TMD's address; *obj0 its first entry. */
static u32 *gs_make_tmd(u32 *t) {
    u32 n = 0, verts0, norms0, prims0, verts1;
    SVECTOR *v;
    u8 *pr;

    t[n++] = 0x41;
    t[n++] = 0; /* flags: offsets */
    t[n++] = 2; /* nobj */
    n += 14;    /* the two entries, filled below */
    verts0 = n;
    v = (SVECTOR *)&t[n];
    /* the F3: on z 0; the G4 on z 500 */
    v[0] = (SVECTOR){ 0, 0, 0, 0 };
    v[1] = (SVECTOR){ 100, 0, 0, 0 };
    v[2] = (SVECTOR){ 0, 100, 0, 0 };
    v[3] = (SVECTOR){ -50, -50, 500, 0 };
    v[4] = (SVECTOR){ 50, -50, 500, 0 };
    v[5] = (SVECTOR){ -50, 50, 500, 0 };
    v[6] = (SVECTOR){ 50, 50, 500, 0 };
    n += 7 * 2;
    norms0 = n;
    ((SVECTOR *)&t[n])[0] = (SVECTOR){ 0, 0, -4096, 0 };
    n += 2;
    prims0 = n;
    pr = (u8 *)&t[n];
    {
        TMD_P_F3 f3 = { 4, 3, 0, 0x20, 200, 100, 50, 0x20, 0, 0, 1, 2 };
        TMD_P_F3 back = { 4, 3, 0, 0x20, 1, 2, 3, 0x20, 0, 0, 2, 1 };
        TMD_P_G4 g4 = { 6, 4, 0, 0x38, 10, 20, 30, 0x38, 0, 3, 0, 4, 0, 5, 0, 6 };

        memcpy(pr, &f3, sizeof(f3));
        memcpy(pr + 16, &back, sizeof(back));
        memcpy(pr + 32, &g4, sizeof(g4));
    }
    n += (16 + 16 + 24) / 4;
    verts1 = n;
    t[n++] = 0;
    t[n++] = 0;
    /* the entries: offsets from the object table (t + 3) */
    t[3] = (verts0 - 3) * 4;
    t[4] = 7;
    t[5] = (norms0 - 3) * 4;
    t[6] = 1;
    t[7] = (prims0 - 3) * 4;
    t[8] = 3;
    t[9] = 0;
    t[10] = (verts1 - 3) * 4;
    t[11] = 1;
    t[12] = (norms0 - 3) * 4;
    t[13] = 1;
    t[14] = (prims0 - 3) * 4;
    t[15] = 0;
    t[16] = 0;
    return t;
}

static PACKET *gs_test_handler_called;
static s32 gs_test_handler_args[3];

/* A program's own handler in a GsFCALL4 entry (the parameters libgs.h documents). */
static PACKET *gs_test_handler(TMD_P_F3 *op, SVECTOR *vp, SVECTOR *np, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                               u32 *scratch) {
    (void)vp;
    (void)np;
    (void)ot;
    (void)scratch;
    gs_test_handler_called = pk;
    gs_test_handler_args[0] = n;
    gs_test_handler_args[1] = shift;
    gs_test_handler_args[2] = op->cd;
    return pk + 4;
}

/* GsMapModelingData, GsLinkObject4 and GsSortObject4 with GsTMDfastF3L and GsTMDfastG4L: the packets in the OT. */
static void test_gs_sort(void) {
    u32 *t = gs_make_tmd(&arena[20000]);
    u32 *org = &arena[22000];
    PACKET *pk = (PACKET *)&arena[24000];
    u32 entry0_before[3] = { t[3], t[5], t[7] }, entry1_before[3] = { t[10], t[12], t[14] };
    GsDOBJ2 obj, obj1;
    GsOT ot;
    MATRIX ls = gs_identity;
    SVECTOR *v;
    s32 sxy[7], p, flag;
    u32 *f3, *g4;
    int i;

    psyq_gs_reset();
    GsInitGraph(320, 240, 0, 0, 0);
    GsInit3D();
    GsSetProjection(1000);
    GsMapModelingData(t + 1);
    CHECK(t[1] == 1, "GsMapModelingData: flags bit 0");
    CHECK(t[3] == entry0_before[0] && t[5] == entry0_before[1] && t[7] == entry0_before[2],
          "GsMapModelingData: object 0's offsets changed");
    CHECK(t[10] == entry1_before[0] - 28 && t[12] == entry1_before[1] - 28 && t[14] == entry1_before[2] - 28,
          "GsMapModelingData: object 1's offsets from its own entry");
    GsMapModelingData(t + 1);
    CHECK(t[10] == entry1_before[0] - 28, "GsMapModelingData twice changed the table");
    memset(&obj, 0, sizeof(obj));
    memset(&obj1, 0, sizeof(obj1));
    GsLinkObject4((uintptr_t)(t + 3), &obj1, 1);
    CHECK(obj1.tmd == t + 10 && (u8 *)obj1.tmd + (s32)obj1.tmd[0] == (u8 *)(t + 3) + entry1_before[0],
          "GsLinkObject4: object 1's entry and its vertices");
    GsLinkObject4((uintptr_t)(t + 3), &obj, 0);
    {
        u8 *pr = (u8 *)(t + 3) + t[7];
        u16 run0, run1;

        memcpy(&run0, pr, 2);
        memcpy(&run1, pr + 32, 2);
        CHECK(obj.tmd == t + 3 && run0 == 2 && run1 == 1, "GsLinkObject4: the runs %u %u", run0, run1);
    }

    /* the view: the identity, 1000 ahead; ambient 1.0 and no light, so a lit colour is the primitive's own */
    GsSetLsMatrix(&ls);
    ls.t[2] = 1000;
    GsSetLsMatrix(&ls);
    GsSetLightMatrix(&ls); /* GsLIGHTWSMATRIX is GsInitGraph's zero: LLM = 0 */
    GsSetAmbient(4096, 4096, 4096);
    ClearOTag(org, 128);
    memset(&ot, 0, sizeof(ot));
    ot.length = 7;
    ot.org = (GsOT_TAG *)org;
    ot.offset = 0;
    GsFCALL4.f3[0][0] = GsTMDfastF3L;
    GsFCALL4.g4[0][0] = GsTMDfastG4L;
    GsSetWorkBase(pk);
    obj.attribute = 0;
    GsSortObject4(&obj, &ot, 2, &arena[26000]);
    CHECK(GsGetWorkBase() == pk + 0x14 + 0x24, "GsSortObject4: the packet area advanced by %d bytes (want 0x38)",
          (int)(GsGetWorkBase() - pk));

    /* the expected screen coordinates, by RotTransPers of each vertex */
    v = (SVECTOR *)((u8 *)(t + 3) + t[3]);
    for (i = 0; i < 7; i++) {
        RotTransPers(&v[i], &sxy[i], &p, &flag);
    }
    CHECK((sxy[1] & 0xFFFF) >= 259 && (sxy[1] & 0xFFFF) <= 260 && (sxy[0] >> 16) == 120, "RotTransPers: %08x %08x",
          (u32)sxy[0], (u32)sxy[1]);
    /* F3: SZ 1000, OTZ = 0x155 * 3000 >> 12 = 249: entry 249 >> 2 = 62 */
    f3 = (u32 *)pk;
    CHECK((org[62] & 0xFFFFFF) == port_ptr_to_u32(f3) && f3[0] == (4u << 24 | port_ptr_to_u32(&org[63])),
          "the F3 in entry 62: entry %06x tag %08x", org[62] & 0xFFFFFF, f3[0]);
    CHECK(f3[1] == 0x203264C8u, "the F3's colour %08x (want its own, code 0x20)", f3[1]);
    CHECK(f3[2] == (u32)sxy[0] && f3[3] == (u32)sxy[1] && f3[4] == (u32)sxy[2], "the F3's vertices");
    /* G4: SZ 1500, OTZ = 0x100 * 6000 >> 12 = 375: entry 93 */
    g4 = (u32 *)(pk + 0x14);
    CHECK((org[93] & 0xFFFFFF) == port_ptr_to_u32(g4) && g4[0] == (8u << 24 | port_ptr_to_u32(&org[94])),
          "the G4 in entry 93: entry %06x tag %08x", org[93] & 0xFFFFFF, g4[0]);
    CHECK(g4[1] == 0x381E140Au && g4[3] == 0x381E140Au && g4[5] == 0x381E140Au && g4[7] == 0x381E140Au,
          "the G4's colours %08x %08x %08x %08x", g4[1], g4[3], g4[5], g4[7]);
    CHECK(g4[2] == (u32)sxy[3] && g4[4] == (u32)sxy[4] && g4[6] == (u32)sxy[5] && g4[8] == (u32)sxy[6],
          "the G4's vertices");
    for (i = 0; i < 127; i++) {
        CHECK(i == 62 || i == 93 || (org[i] & 0xFFFFFF) == port_ptr_to_u32(&org[i + 1]), "entry %d changed", i);
    }
    /* drawn: the F3 at (160, 120) (260, 120) (160, 220), the G4 33 around the centre (entry 93, after the F3); the
     * F3's back-facing twin drew nothing */
    {
        const u16 *vram = psyq_gpu_vram();
        DRAWENV env;

        ResetGraph(0);
        SetDefDrawEnv(&env, 0, 0, 320, 240);
        env.dtd = 0;
        env.isbg = 1;
        PutDrawEnv(&env);
        DrawOTag(org);
        CHECK(vram[125 * 1024 + 240] == ((50 >> 3) << 10 | (100 >> 3) << 5 | (200 >> 3)), "the F3 drawn: %04x",
              vram[125 * 1024 + 240]);
        CHECK(vram[100 * 1024 + 150] == ((30 >> 3) << 10 | (20 >> 3) << 5 | (10 >> 3)), "the G4 drawn: %04x",
              vram[100 * 1024 + 150]);
        CHECK(vram[118 * 1024 + 240] == 0 && vram[200 * 1024 + 200] == 0, "drawn outside the polygons");
    }

    /* GsDOFF: nothing; a semi-transparent object: the code's bit 1 */
    GsSetWorkBase(pk);
    obj.attribute = 0x80000000u;
    GsSortObject4(&obj, &ot, 2, &arena[26000]);
    CHECK(GsGetWorkBase() == pk, "GsSortObject4 drew a GsDOFF object");
    obj.attribute = 1u << 30;
    GsSortObject4(&obj, &ot, 2, &arena[26000]);
    CHECK(((u32 *)pk)[1] == 0x223264C8u, "GsALON: the F3's code %08x", ((u32 *)pk)[1]);

    /* the table's choice: lighting off (GsLOFF) takes [0][2]; GsDIV takes [1][...] and fills scratch; the program's
     * own handler gets the run (n 2), shift and the primitive */
    memset(&GsFCALL4, 0, sizeof(GsFCALL4));
    GsFCALL4.f3[0][2] = gs_test_handler;
    GsFCALL4.f3[1][0] = gs_test_handler;
    GsFCALL4.g4[0][2] = GsTMDfastG4L;
    GsFCALL4.g4[1][0] = GsTMDfastG4L;
    GsSetWorkBase(pk);
    gs_test_handler_called = NULL;
    obj.attribute = 1u << 6;
    GsSortObject4(&obj, &ot, 3, &arena[26000]);
    CHECK(gs_test_handler_called == pk && gs_test_handler_args[0] == 2 && gs_test_handler_args[1] == 3 &&
          gs_test_handler_args[2] == 0x20, "GsLOFF: the handler in f3[0][2] (n %d shift %d)", gs_test_handler_args[0],
          gs_test_handler_args[1]);
    gs_test_handler_called = NULL;
    arena[26000] = arena[26001] = arena[26002] = 0;
    obj.attribute = 3u << 9;
    GsSortObject4(&obj, &ot, 2, &arena[26000]);
    CHECK(gs_test_handler_called != NULL && arena[26000] == 3 && arena[26001] == 320 && arena[26002] == 240,
          "GsDIV: f3[1][0] and scratch %u %u %u", arena[26000], arena[26001], arena[26002]);
    /* GsSetLightMode(1) (fog) takes [..][1]; with GsLLMOD the attribute's mode decides */
    GsFCALL4.f3[0][1] = gs_test_handler;
    GsFCALL4.f3[0][0] = NULL;
    GsFCALL4.g4[0][1] = GsTMDfastG4L;
    GsSetLightMode(1);
    gs_test_handler_called = NULL;
    obj.attribute = 0;
    GsSortObject4(&obj, &ot, 2, &arena[26000]);
    CHECK(gs_test_handler_called != NULL, "GsSetLightMode(1): f3[0][1]");
    GsFCALL4.f3[0][0] = gs_test_handler;
    GsFCALL4.f3[0][1] = NULL;
    GsFCALL4.g4[0][0] = GsTMDfastG4L;
    gs_test_handler_called = NULL;
    obj.attribute = 1u << 5;
    GsSortObject4(&obj, &ot, 2, &arena[26000]);
    CHECK(gs_test_handler_called != NULL, "GsLLMOD with mode 0: f3[0][0] despite GsSetLightMode(1)");
    GsSetLightMode(0);
}

/* ---- LIBC2's rand and srand (psyq/libc2.c) ---- */

/* The host's rand, reached by its own name: the shim's must not replace it (psyq_names.h renames the PS1's). */
static int host_rand_after_srand1(void) {
    srand(1);
    return rand();
}

/* A save state's blocks in memory (savestate.h; runtime/savestate.c is not linked here): saving appends, loading
 * reads them back in the same order. */
struct PortState {
    int loading;
    u8 *buf;
    size_t len, cap, pos;
};

void port_state_bytes(PortState *s, const char *tag, void *data, size_t size) {
    (void)tag;
    if (s->loading) {
        memcpy(data, s->buf + s->pos, size);
        s->pos += size;
        return;
    }
    if (s->len + size > s->cap) {
        s->cap = (s->len + size) * 2;
        s->buf = realloc(s->buf, s->cap);
    }
    memcpy(s->buf + s->len, data, size);
    s->len += size;
}

int port_state_loading(const PortState *s) {
    return s->loading;
}

/* From here the game's view: psyq_names.h sends rand and srand to the shim's psyq_c2_*, as in a game's unit. */
#include "psxstack/psyq_names.h"
s32 rand(void);
void srand(u32 seed);

static void test_libc2(void) {
    /* the generator's definition, from seed 1: state = state * 0x41C64E6D + 12345, bits 16-30 */
    static const s32 from1[10] = { 16838, 5758, 10113, 17515, 31051, 5627, 23010, 7419, 16212, 4086 };
    s32 got[10], again[10], max = 0, min = 0x7FFF;
    u32 st = 0;
    int i, glibc;
    PortState save = { 0, NULL, 0, 0, 0 };

    /* power-on: the state is 0 (the PS1's .bss), so the first draw is 12345 >> 16 = 0, the second 21468 */
    psyq_reset();
    CHECK(psyq_rand_seed() == 0, "the state after the reset is %u, want 0", psyq_rand_seed());
    CHECK(rand() == 0 && psyq_rand_seed() == 12345u, "the first draw from power-on");
    CHECK(rand() == 21468, "the second draw from power-on");
    srand(1);
    for (i = 0; i < 10; i++) {
        got[i] = rand();
        CHECK(got[i] == from1[i], "rand #%d after srand(1) = %d, want %d", i, got[i], from1[i]);
    }
    /* srand's round trip: the same seed, the same sequence; the state readable as the PS1 keeps it */
    srand(1);
    for (i = 0; i < 10; i++) {
        again[i] = rand();
    }
    CHECK(memcmp(got, again, sizeof(got)) == 0, "srand(1) again does not repeat the sequence");
    srand(0xDEADBEEFu);
    CHECK(psyq_rand_seed() == 0xDEADBEEFu, "srand's seed reads back as %#x", psyq_rand_seed());
    st = 0xDEADBEEFu * 0x41C64E6Du + 12345u;
    CHECK(rand() == (s32)((st >> 16) & 0x7FFF) && psyq_rand_seed() == st, "a draw from 0xDEADBEEF");
    /* RAND_MAX is 0x7FFF: 2^17 draws stay in 0..0x7FFF and reach both ends */
    for (i = 0; i < (1 << 17); i++) {
        s32 r = rand();
        max = r > max ? r : max;
        min = r < min ? r : min;
    }
    CHECK(min == 0 && max == 0x7FFF, "2^17 draws span %d..%d, want 0..0x7FFF", min, max);
    /* the console's reset clears the state; a save state holds it */
    srand(77);
    psyq_reset();
    CHECK(psyq_rand_seed() == 0 && rand() == 0, "the reset did not clear the state");
    srand(1);
    rand();
    psyq_state(&save);
    for (i = 0; i < 5; i++) {
        got[i] = rand();
    }
    srand(999);
    save.loading = 1;
    psyq_state(&save);
    CHECK(save.pos == save.len, "the state read %zu of %zu bytes", save.pos, save.len);
    for (i = 0; i < 5; i++) {
        again[i] = rand();
    }
    CHECK(memcmp(got, again, 5 * sizeof(s32)) == 0 && got[0] == from1[1], "a loaded state does not resume rand");
    free(save.buf);
    /* the host's rand is still libc's (another generator, RAND_MAX above 0x7FFF on glibc) */
    glibc = host_rand_after_srand1();
    CHECK(glibc != from1[0], "the host's rand gave the PS1's first value: the shim's replaced libc's");
    psyq_reset();
}

int main(void) {
    psyq_reset();
    test_scalar();
    test_matrices();
    test_transforms();
    test_packets();
    test_addprim();
    test_images();
    test_envs();
    test_tim();
    test_gs_view();
    test_gs_coords();
    test_gs_setup();
    test_gs_sort();
    test_libc2();
    printf("psyq_test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
