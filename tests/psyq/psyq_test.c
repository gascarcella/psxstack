/* tests/psyq/psyq_test.c: the shim's LIBGTE and LIBGPU functions on their own (tests/psyq_test.py builds this with the
 * shim's sources and the flags of psyq/check.sh, and runs it). Documented cases and properties, no game and no disc:
 * the values a function must give by its definition (Sony's descriptions, psx-spx), a function against the commands it
 * is made of, and the packets' bytes. What only the PS1 can settle (the exact rounding of the CORDICs, FLAG in corner
 * cases) is the consumers' goldens' (psyq/README.md "Behaviour assumed"). Prints one line per failure; exit 1 on any. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psyq_internal.h"
#include "psxstack/spu.h"
#include "psxstack/psyq/libgpu.h"
#include "psxstack/psyq/libgte.h"

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
    printf("psyq_test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
