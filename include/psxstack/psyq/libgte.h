#ifndef PSYQ_LIBGTE_H
#define PSYQ_LIBGTE_H

/* psxstack's declarations of the Psy-Q 4.7 LIBGTE interface, as its games use it: written from the games' use of the
 * API and public documentation, no Sony header (DECISIONS "Psy-Q declarations: the stack's"). The shim implements
 * these; a game's own recovered declarations must agree with them in ABI (tools/psyq_decls.py). */

#include "psxstack/types.h"

typedef struct {
    u8 r, g, b, cd;
} CVECTOR;

typedef struct {
    s16 m[3][3];
    s32 t[3];
} MATRIX;

typedef struct {
    s32 vx, vy, vz, pad;
} VECTOR;

typedef struct {
    s16 vx, vy, vz, pad;
} SVECTOR;

typedef struct {
    s16 vx, vy;
} DVECTOR;

void InitGeom(void);
void SetGeomOffset(s32 ofx, s32 ofy);
MATRIX *RotMatrixYXZ_gte(SVECTOR *r, MATRIX *m);
MATRIX *RotMatrixZYX_gte(SVECTOR *r, MATRIX *m);
MATRIX *ScaleMatrix(MATRIX *m, VECTOR *v);
SVECTOR *ApplyMatrixSV(MATRIX *m, SVECTOR *v0, SVECTOR *v1);
void SetBackColor(s32 rbk, s32 gbk, s32 bbk);
s32 rsin(s32 a);
s32 rcos(s32 a);
void SetGeomScreen(s32 h);
void SetFarColor(s32 rfc, s32 gfc, s32 bfc);
void SetColorMatrix(MATRIX *m);
MATRIX *MulMatrix(MATRIX *m0, MATRIX *m1);
MATRIX *MulMatrix2(MATRIX *m0, MATRIX *m1);
VECTOR *ApplyMatrixLV(MATRIX *m, VECTOR *v0, VECTOR *v1);
MATRIX *TransposeMatrix(MATRIX *m0, MATRIX *m1);
s32 SquareRoot0(s32 a);

/* The register setters, the matrix stack (20 entries of RT and TR) and the matrix helpers. */
void SetRotMatrix(MATRIX *m);
void SetLightMatrix(MATRIX *m);
void SetTransMatrix(MATRIX *m);
MATRIX *TransMatrix(MATRIX *m, VECTOR *v);
void PushMatrix(void);
void PopMatrix(void);
MATRIX *MulMatrix0(MATRIX *m0, MATRIX *m1, MATRIX *m2);
MATRIX *CompMatrix(MATRIX *m0, MATRIX *m1, MATRIX *m2);
MATRIX *RotMatrix(SVECTOR *r, MATRIX *m);
MATRIX *RotMatrixYXZ(SVECTOR *r, MATRIX *m);
void MatrixNormal(MATRIX *m, MATRIX *n);
void VectorNormal(VECTOR *v0, VECTOR *v1);

/* The perspective transforms (sxy: SX | SY << 16; p: the depth-cue factor; otz: the depth / 4; flag: FLAG). */
s32 RotTransPers(SVECTOR *v0, s32 *sxy, s32 *p, s32 *flag);
s32 RotTransPers3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *p, s32 *flag);
s32 RotTransPers4(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, SVECTOR *v3, s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *sxy3,
                  s32 *p, s32 *flag);
void RotTrans(SVECTOR *v0, VECTOR *v1, s32 *flag);
s32 RotAverage4(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, SVECTOR *v3, s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *sxy3,
                s32 *p, s32 *flag);
s32 RotNclip3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *p, s32 *otz, s32 *flag);
s32 RotNclip4(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, SVECTOR *v3, s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *sxy3, s32 *p,
              s32 *otz, s32 *flag);
s32 RotAverageNclip3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *p, s32 *otz,
                     s32 *flag);
s32 RotAverageNclip4(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, SVECTOR *v3, s32 *sxy0, s32 *sxy1, s32 *sxy2, s32 *sxy3,
                     s32 *p, s32 *otz, s32 *flag);

/* Lighting. */
void NormalColorCol(SVECTOR *v0, CVECTOR *v1, CVECTOR *v2);
void NormalColorCol3(SVECTOR *v0, SVECTOR *v1, SVECTOR *v2, CVECTOR *v3, CVECTOR *v4, CVECTOR *v5, CVECTOR *v6);

/* Scalar maths: 4096 = 1.0, 4096 = a turn. csqrt and catan are also C99's complex functions (<complex.h>), which
 * compilers know as built-ins: LIBGTE's are a different function under the same name, so the mismatch warning is off
 * for their declarations (the shim's definitions are what the game's calls link to; its units build with -fno-builtin). */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wincompatible-library-redeclaration"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wbuiltin-declaration-mismatch"
#endif
s32 csqrt(s32 a);
s32 catan(s32 a);
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
s32 ratan2(s32 y, s32 x);

#endif /* PSYQ_LIBGTE_H */
