#ifndef PSYQ_LIBGS_H
#define PSYQ_LIBGS_H

/* psxstack's declarations of the Psy-Q 4.7 LIBGS interface, as its games use it: written from the games' use of the
 * API and public documentation, no Sony header (DECISIONS "Psy-Q declarations: the stack's"). The shim implements
 * these; a game's own recovered declarations must agree with them in ABI (tools/psyq_decls.py). */

#include <stdint.h>

#include "psxstack/types.h"
#include "psxstack/psyq/libgte.h"

/* GsGetTimInfo's result. */
typedef struct {
    /* 0x00 */ u32 pmode;
    /* 0x04 */ s16 px;
    /* 0x06 */ s16 py;
    /* 0x08 */ u16 pw;
    /* 0x0A */ u16 ph;
    /* 0x0C */ u32 *pixel;
    /* 0x10 */ s16 cx;
    /* 0x12 */ s16 cy;
    /* 0x14 */ u16 cw;
    /* 0x16 */ u16 ch;
    /* 0x18 */ u32 *clut;
} GsIMAGE; /* size 0x1C */

/* A flat (directional) light. */
typedef struct {
    s32 vx, vy, vz;
    u8 r, g, b;
} GsF_LIGHT;

void GsInitGraph(u16 w, u16 h, u16 intl, u16 dither, u16 vram);
void GsGetTimInfo(u32 *tim, GsIMAGE *image);
void GsSetProjection(s32 h);
void GsInit3D(void);
s32 GsSetFlatLight(s32 id, GsF_LIGHT *lt);
void GsSetLightMode(s32 mode);

/* A coordinate system (a node of a hierarchy). */
typedef struct _GsCOORDINATE2 {
    /* 0x00 */ u32 flg;
    /* 0x04 */ MATRIX coord;
    /* 0x24 */ MATRIX workm;
    /* 0x44 */ void *param; /* GsCOORD2PARAM */
    /* 0x48 */ struct _GsCOORDINATE2 *super;
    /* 0x4C */ struct _GsCOORDINATE2 *sub;
} GsCOORDINATE2; /* size 0x50 */

/* A view: viewpoint, reference point, twist, and the coordinate system they are in. */
typedef struct {
    /* 0x00 */ s32 vpx, vpy, vpz;
    /* 0x0C */ s32 vrx, vry, vrz;
    /* 0x18 */ s32 rz;
    /* 0x1C */ GsCOORDINATE2 *super;
} GsRVIEW2; /* size 0x20 */

s32 GsSetRefView2(GsRVIEW2 *pv);

/* ---- the coordinate systems and the GTE's matrices ---- */
extern MATRIX GsIDMATRIX;      /* the identity (GsInitGraph); GsInitCoordinate2 copies it */
extern MATRIX GsWSMATRIX;      /* the world-screen matrix (GsSetRefView2) */
extern MATRIX GsLIGHTWSMATRIX; /* the flat-light matrix (GsSetFlatLight) */

void GsInitCoordinate2(GsCOORDINATE2 *super, GsCOORDINATE2 *base);
void GsGetLw(GsCOORDINATE2 *coord, MATRIX *m);
void GsGetLs(GsCOORDINATE2 *coord, MATRIX *m);
void GsGetLws(GsCOORDINATE2 *coord, MATRIX *lw, MATRIX *ls);
void GsMulCoord3(MATRIX *m1, MATRIX *m2);
void GsSetLsMatrix(MATRIX *m);
void GsSetLightMatrix(MATRIX *m);
void GsSetAmbient(s32 r, s32 g, s32 b);
void GsSwapDispBuff(void);

/* ---- objects: TMD data, the packet area, the ordering table ---- */
typedef u8 PACKET;

/* An ordering-table entry: the 24-bit link and the primitive's word count, as P_TAG. */
typedef struct {
    unsigned p : 24;
    unsigned num : 8;
} GsOT_TAG;

/* An ordering table: GsSortObject4 links a primitive of depth z into org[((z - offset) >> shift) & 0xFFFF]. */
typedef struct {
    u32 length;
    GsOT_TAG *org;
    u32 offset;
    u32 point;
    GsOT_TAG *tag;
} GsOT;

/* An object: GsLinkObject4 sets tmd (the object's entry in the TMD's object table); the caller sets the rest.
 * attribute: bit 31 GsDOFF (not drawn), bit 30 GsALON (semi-transparent), bits 9-11 GsDIV (subdivision), bit 6 GsLOFF
 * (no lighting), bit 5 GsLLMOD (bits 3-4 choose the lighting mode, else GsSetLightMode's), bits 3-4 the lighting mode
 * (bit 3: fog), bits 0-2 the material attenuation. */
typedef struct {
    u32 attribute;
    GsCOORDINATE2 *coord2;
    u32 *tmd;
    u32 id;
} GsDOBJ2;

/* A TMD (the PS1 model format): {u32 id 0x41, flags, nobj} and nobj object entries of 7 words, {vert_top, n_vert,
 * normal_top, n_normal, primitive_top, n_primitive, scale}; the three *_top words are offsets from the object table
 * (flags bit 0 clear) or, on the PS1 after GsMapModelingData (bit 0 set), addresses. A 32-bit word cannot hold a host
 * address, so on the host GsMapModelingData leaves offsets there, each relative to its own object's entry (the word
 * minus 28 * the object's index: object 0's are the file's offsets unchanged), and sets bit 0 as the PS1 does; the
 * shim resolves a word as (u8 *)entry + (s32)word (psyq/README.md "Behaviour assumed", LIBGS). A TMD whose flags
 * already have bit 0 set holds PS1 addresses the host cannot use. GsLinkObject4 also rewrites the first halfword of
 * each run of primitives of the same mode and flag as the run's length, which GsSortObject4 reads.
 * The primitives (a 4-byte header {olen, ilen, flag, mode}, then the mode's fields): */
typedef struct {
    u8 out, in, dummy, cd;
    u8 r0, g0, b0, code;
    u16 n0, v0;
    u16 v1, v2;
} TMD_P_F3;

typedef struct {
    u8 out, in, dummy, cd;
    u8 r0, g0, b0, code;
    u16 n0, v0;
    u16 n1, v1;
    u16 n2, v2;
} TMD_P_G3;

typedef struct {
    u8 out, in, dummy, cd;
    u8 r0, g0, b0, code;
    u16 n0, v0;
    u16 v1, v2;
    u16 v3, p;
} TMD_P_F4;

typedef struct {
    u8 out, in, dummy, cd;
    u8 r0, g0, b0, code;
    u16 n0, v0;
    u16 n1, v1;
    u16 n2, v2;
    u16 n3, v3;
} TMD_P_G4;

typedef struct {
    u8 out, in, dummy, cd;
    u8 tu0, tv0;
    u16 clut;
    u8 tu1, tv1;
    u16 tpage;
    u8 tu2, tv2;
    u16 p;
    u16 n0, v0;
    u16 v1, v2;
} TMD_P_TF3;

typedef struct {
    u8 out, in, dummy, cd;
    u8 tu0, tv0;
    u16 clut;
    u8 tu1, tv1;
    u16 tpage;
    u8 tu2, tv2;
    u16 p;
    u16 n0, v0;
    u16 n1, v1;
    u16 n2, v2;
} TMD_P_TG3;

typedef struct {
    u8 out, in, dummy, cd;
    u8 tu0, tv0;
    u16 clut;
    u8 tu1, tv1;
    u16 tpage;
    u8 tu2, tv2;
    u16 p0;
    u8 tu3, tv3;
    u16 p1;
    u16 n0, v0;
    u16 v1, v2;
    u16 v3, p2;
} TMD_P_TF4;

typedef struct {
    u8 out, in, dummy, cd;
    u8 tu0, tv0;
    u16 clut;
    u8 tu1, tv1;
    u16 tpage;
    u8 tu2, tv2;
    u16 p0;
    u8 tu3, tv3;
    u16 p1;
    u16 n0, v0;
    u16 n1, v1;
    u16 n2, v2;
    u16 n3, v3;
} TMD_P_TG4;

typedef struct {
    u8 out, in, dummy, cd;
    u8 r0, g0, b0, code;
    u16 v0, v1;
    u16 v2, v3;
} TMD_P_NF4;

typedef struct {
    u8 out, in, dummy, cd;
    u8 tu0, tv0;
    u16 clut;
    u8 tu1, tv1;
    u16 tpage;
    u8 tu2, tv2;
    u16 p0;
    u8 r0, g0, b0, p1;
    u16 v0, v1;
    u16 v2, p2;
} TMD_P_TNF3;

typedef struct {
    u8 out, in, dummy, cd;
    u8 tu0, tv0;
    u16 clut;
    u8 tu1, tv1;
    u16 tpage;
    u8 tu2, tv2;
    u16 p0;
    u8 r0, g0, b0, p1;
    u8 r1, g1, b1, p2;
    u8 r2, g2, b2, p3;
    u16 v0, v1;
    u16 v2, p4;
} TMD_P_TNG3;

typedef struct {
    u8 out, in, dummy, cd;
    u8 tu0, tv0;
    u16 clut;
    u8 tu1, tv1;
    u16 tpage;
    u8 tu2, tv2;
    u16 p0;
    u8 tu3, tv3;
    u16 p1;
    u8 r0, g0, b0, p2;
    u16 v0, v1;
    u16 v2, v3;
} TMD_P_TNF4;

typedef struct {
    u8 out, in, dummy, cd;
    u8 tu0, tv0;
    u16 clut;
    u8 tu1, tv1;
    u16 tpage;
    u8 tu2, tv2;
    u16 p0;
    u8 tu3, tv3;
    u16 p1;
    u8 r0, g0, b0, p2;
    u8 r1, g1, b1, p3;
    u8 r2, g2, b2, p4;
    u8 r3, g3, b3, p5;
    u16 v0, v1;
    u16 v2, v3;
} TMD_P_TNG4;

void GsMapModelingData(u32 *p);
void GsLinkObject4(uintptr_t tmd_base, GsDOBJ2 *objp, s32 n);

/* The packet area GsSortObject4 writes its primitives to (GsSetWorkBase), and where it stopped (GsGetWorkBase). */
extern PACKET *GsOUT_PACKET_P;
void GsSetWorkBase(PACKET *base);
PACKET *GsGetWorkBase(void);

/* GsSortObject4's jump table: the primitive handlers, by primitive type, [GsDIV != 0][lighting: 0 normal, 1 fog,
 * 2 off] (the n* entries, primitives without normals, by [GsDIV != 0]; the *g entries, F/G primitives with gradation,
 * by lighting). LIBGS leaves it zero: the program stores the handlers it links (Sony's GsTMDfast and GsTMDdiv functions
 * below, or its own with the same parameters). A handler takes `n` primitives of its type from `primtop` and returns
 * the packet area's new end; those with normals are called (primtop, vertop, nortop, pk, n, shift, ot, scratch), those
 * without (primtop, vertop, pk, n, shift, ot, scratch). */
typedef struct {
    PACKET *(*f3[2][3])();
    PACKET *(*nf3[2])();
    PACKET *(*g3[2][3])();
    PACKET *(*ng3[2])();
    PACKET *(*tf3[2][3])();
    PACKET *(*ntf3[2])();
    PACKET *(*tg3[2][3])();
    PACKET *(*ntg3[2])();
    PACKET *(*f4[2][3])();
    PACKET *(*nf4[2])();
    PACKET *(*g4[2][3])();
    PACKET *(*ng4[2])();
    PACKET *(*tf4[2][3])();
    PACKET *(*ntf4[2])();
    PACKET *(*tg4[2][3])();
    PACKET *(*ntg4[2])();
    PACKET *(*f3g[3])();
    PACKET *(*g3g[3])();
    PACKET *(*f4g[3])();
    PACKET *(*g4g[3])();
} _GsFCALL;

extern _GsFCALL GsFCALL4;
void GsSortObject4(GsDOBJ2 *objp, GsOT *otp, s32 shift, u32 *scratch);

/* The handlers (psyq/libgs_sort.c): "fast" ones draw each primitive as one polygon; "L" ones light it with the GTE
 * (NCCS/NCCT over the primitive's normals), "NL" ones do not (lighting off), N ones have no normals (their colours are
 * in the primitive). The GsTMDdiv ones subdivide (GsDIV) and are not in the shim yet: each stops the run with
 * port_unimplemented. */
PACKET *GsTMDfastF3L(TMD_P_F3 *primtop, SVECTOR *vertop, SVECTOR *nortop, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                     u32 *scratch);
PACKET *GsTMDfastG3L(TMD_P_G3 *primtop, SVECTOR *vertop, SVECTOR *nortop, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                     u32 *scratch);
PACKET *GsTMDfastF4L(TMD_P_F4 *primtop, SVECTOR *vertop, SVECTOR *nortop, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                     u32 *scratch);
PACKET *GsTMDfastF4NL(TMD_P_F4 *primtop, SVECTOR *vertop, SVECTOR *nortop, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                      u32 *scratch);
PACKET *GsTMDfastG4L(TMD_P_G4 *primtop, SVECTOR *vertop, SVECTOR *nortop, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                     u32 *scratch);
PACKET *GsTMDfastTF3L(TMD_P_TF3 *primtop, SVECTOR *vertop, SVECTOR *nortop, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                      u32 *scratch);
PACKET *GsTMDfastTF3NL(TMD_P_TF3 *primtop, SVECTOR *vertop, SVECTOR *nortop, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                       u32 *scratch);
PACKET *GsTMDfastTG3L(TMD_P_TG3 *primtop, SVECTOR *vertop, SVECTOR *nortop, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                      u32 *scratch);
PACKET *GsTMDfastTG3NL(TMD_P_TG3 *primtop, SVECTOR *vertop, SVECTOR *nortop, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                       u32 *scratch);
PACKET *GsTMDfastTF4L(TMD_P_TF4 *primtop, SVECTOR *vertop, SVECTOR *nortop, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                      u32 *scratch);
PACKET *GsTMDfastTF4NL(TMD_P_TF4 *primtop, SVECTOR *vertop, SVECTOR *nortop, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                       u32 *scratch);
PACKET *GsTMDfastTG4L(TMD_P_TG4 *primtop, SVECTOR *vertop, SVECTOR *nortop, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                      u32 *scratch);
PACKET *GsTMDfastTG4NL(TMD_P_TG4 *primtop, SVECTOR *vertop, SVECTOR *nortop, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                       u32 *scratch);
PACKET *GsTMDfastNF4(TMD_P_NF4 *primtop, SVECTOR *vertop, PACKET *pk, s32 n, s32 shift, GsOT *ot, u32 *scratch);
PACKET *GsTMDfastTNF3(TMD_P_TNF3 *primtop, SVECTOR *vertop, PACKET *pk, s32 n, s32 shift, GsOT *ot, u32 *scratch);
PACKET *GsTMDfastTNG3(TMD_P_TNG3 *primtop, SVECTOR *vertop, PACKET *pk, s32 n, s32 shift, GsOT *ot, u32 *scratch);
PACKET *GsTMDfastTNF4(TMD_P_TNF4 *primtop, SVECTOR *vertop, PACKET *pk, s32 n, s32 shift, GsOT *ot, u32 *scratch);
PACKET *GsTMDfastTNG4(TMD_P_TNG4 *primtop, SVECTOR *vertop, PACKET *pk, s32 n, s32 shift, GsOT *ot, u32 *scratch);
PACKET *GsTMDdivTF3NL(TMD_P_TF3 *primtop, SVECTOR *vertop, SVECTOR *nortop, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                      u32 *scratch);
PACKET *GsTMDdivTG3NL(TMD_P_TG3 *primtop, SVECTOR *vertop, SVECTOR *nortop, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                      u32 *scratch);
PACKET *GsTMDdivTF4L(TMD_P_TF4 *primtop, SVECTOR *vertop, SVECTOR *nortop, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                     u32 *scratch);
PACKET *GsTMDdivTF4NL(TMD_P_TF4 *primtop, SVECTOR *vertop, SVECTOR *nortop, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                      u32 *scratch);
PACKET *GsTMDdivTG4NL(TMD_P_TG4 *primtop, SVECTOR *vertop, SVECTOR *nortop, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                      u32 *scratch);
PACKET *GsTMDdivTNF3(TMD_P_TNF3 *primtop, SVECTOR *vertop, PACKET *pk, s32 n, s32 shift, GsOT *ot, u32 *scratch);
PACKET *GsTMDdivTNG3(TMD_P_TNG3 *primtop, SVECTOR *vertop, PACKET *pk, s32 n, s32 shift, GsOT *ot, u32 *scratch);
PACKET *GsTMDdivTNF4(TMD_P_TNF4 *primtop, SVECTOR *vertop, PACKET *pk, s32 n, s32 shift, GsOT *ot, u32 *scratch);
PACKET *GsTMDdivTNG4(TMD_P_TNG4 *primtop, SVECTOR *vertop, PACKET *pk, s32 n, s32 shift, GsOT *ot, u32 *scratch);

#endif /* PSYQ_LIBGS_H */
