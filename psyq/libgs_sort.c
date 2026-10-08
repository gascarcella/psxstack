/* psyq/libgs_sort.c: LIBGS's drawing. GsInit3D and GsSwapDispBuff (the double buffer over LIBGPU's environments),
 * GsLinkObject4 and GsSortObject4 (a TMD object's primitives through the GsFCALL4 jump table into an ordering table),
 * and the "fast" primitive handlers a program stores in that table. Each handler issues the GTE commands of the PS1's
 * LIBGS 4.x object in its order (RTPT, NCLIP, RTPS for a quad's fourth vertex, AVSZ3/4, NCCS/NCCT for the lit ones),
 * with its register loads, its tests (FLAG's error bit after each perspective transform, the back face: NCLIP's
 * MAC0 <= 0) and its packet words, so the primitives and the GTE registers left behind are the console's; not yet
 * checked against an emulator (psyq/README.md "Behaviour assumed", LIBGS). The subdividing GsTMDdiv handlers are not
 * here yet: each stops the run with port_unimplemented.
 *
 * Two host differences, neither visible to the game: the PS1 handlers read the next primitive's indices ahead (past
 * the last one), these do not; and the screen coordinates go through a four-word vertex cache (swc2, consecutive)
 * before the packet, as the games' own meshes do, so that the sub-pixel shadow (gte_shadow.c) finds them when the
 * packet is linked (port_ptr_to_u32); the PS1 stores them into the packet directly. */
#include <stddef.h>
#include <string.h>

#include "psyq_internal.h"
#include "libgs_internal.h"

/* ---- the double buffer ---- */

/* GsSetDrawBuffClip: the drawing environment's clip is the clip rectangle in the buffer drawn; put. */
static void psyq_gs_set_draw_buff_clip(void) {
    psyq_gs.draw.clip.x = (s16)(psyq_gs.clip.x + psyq_gs.buf_x[psyq_gs.idx]);
    psyq_gs.draw.clip.y = (s16)(psyq_gs.clip.y + psyq_gs.buf_y[psyq_gs.idx]);
    psyq_gs.draw.clip.w = psyq_gs.clip.w;
    psyq_gs.draw.clip.h = psyq_gs.clip.h;
    PutDrawEnv(&psyq_gs.draw);
}

/* GsSetDrawBuffOffset: the screen's origin in the buffer drawn, to the GPU (GsOFSGPU: the drawing environment's
 * offset, put; the GTE's offset is then 0, 0) or to the GTE (SetGeomOffset). The GTE's case adds the other buffer's
 * origin, as the PS1 does (both are 0, 0 without GsDefDispBuff). */
static void psyq_gs_set_draw_buff_offset(void) {
    if (psyq_gs.ofsgpu != 0) {
        psyq_gs.draw.ofs[0] = (s16)(psyq_gs.draw_ofs[0] + psyq_gs.buf_x[psyq_gs.idx]);
        psyq_gs.draw.ofs[1] = (s16)(psyq_gs.draw_ofs[1] + psyq_gs.buf_y[psyq_gs.idx]);
        psyq_gs.gte_ofs[0] = psyq_gs.gte_ofs[1] = 0;
        PutDrawEnv(&psyq_gs.draw);
    } else {
        s32 x = psyq_gs.draw_ofs[0] + psyq_gs.buf_x[psyq_gs.idx == 0];
        s32 y = psyq_gs.draw_ofs[1] + psyq_gs.buf_y[psyq_gs.idx == 0];

        SetGeomOffset(x, y);
        psyq_gs.gte_ofs[0] = (s16)x;
        psyq_gs.gte_ofs[1] = (s16)y;
    }
}

/* Real (gs_104.s): the screen's origin is its centre (w / 2, h / 2) and goes to the GTE (the games' intl has no
 * GsOFSGPU bit: SetGeomOffset(160, 120) for 320x240; they set it again themselves); then the light mode is 0 and
 * LIBGS's Z range 10..0x3FFF. No matrix is touched: the light matrix stays GsInitGraph's zero and the world-screen
 * matrix zero until GsSetRefView2. */
void GsInit3D(void) {
    PSYQ_TRACE("GsInit3D");
    psyq_gs.draw_ofs[0] = (s16)((s32)psyq_gs.w / 2);
    psyq_gs.draw_ofs[1] = (s16)((s32)psyq_gs.h / 2);
    psyq_gs_set_draw_buff_offset();
    psyq_gs.zmin = 10;
    psyq_gs.light_mode = 0;
    psyq_gs.zmax = 0x3FFF;
}

/* Real: shows the buffer just drawn (PutDispEnv of LIBGS's display environment at its origin, SetDispMask(1)), counts
 * PSDCNT (skipping 0: every GsCOORDINATE2's cached workm is stale after it), makes the other buffer the one drawn and
 * puts the drawing environment's clip and offset for it. */
void GsSwapDispBuff(void) {
    PSYQ_TRACE("GsSwapDispBuff buffer %d", psyq_gs.idx);
    psyq_gs.disp.disp.x = psyq_gs.buf_x[psyq_gs.idx];
    psyq_gs.disp.disp.y = psyq_gs.buf_y[psyq_gs.idx];
    PutDispEnv(&psyq_gs.disp);
    SetDispMask(1);
    D_800812D8++;
    if (D_800812D8 == 0) {
        D_800812D8 = 1;
    }
    psyq_gs.idx = (s16)(psyq_gs.idx == 0);
    psyq_gs_set_draw_buff_clip();
    psyq_gs_set_draw_buff_offset();
}

/* ---- objects ---- */

/* A TMD object table word (vert_top, normal_top, primitive_top) in the host's form: an offset from the object's entry
 * (GsMapModelingData, libgs.h). */
static void *psyq_gs_tmd(const u32 *entry, u32 word) {
    return (u8 *)entry + (s32)word;
}

static u32 psyq_gs_word(const void *p) {
    u32 w;

    memcpy(&w, p, 4);
    return w;
}

static void psyq_gs_put_word(void *p, u32 w) {
    memcpy(p, &w, 4);
}

/* A primitive's size in bytes from its mode (bit 1, semi-transparency, ignored) and flag (bit 2, GRD: one colour per
 * vertex for the F and G types); 0 for a mode LIBGS does not draw. */
static u32 psyq_gs_prim_size(u32 mode, u32 flag) {
    int grd = (flag & 4) != 0;

    switch (mode & 0xFD) {
    case 0x20: return grd ? 0x18 : 0x10; /* F3 */
    case 0x21: return 0x10;              /* NF3 */
    case 0x24: return 0x18;              /* TF3 */
    case 0x25: return 0x1C;              /* TNF3 */
    case 0x28: return grd ? 0x20 : 0x14; /* F4 */
    case 0x29: return 0x10;              /* NF4 */
    case 0x2C: return 0x20;              /* TF4 */
    case 0x2D: return 0x20;              /* TNF4 */
    case 0x30: return grd ? 0x1C : 0x14; /* G3 */
    case 0x31: return 0x18;              /* NG3 */
    case 0x34: return 0x1C;              /* TG3 */
    case 0x35: return 0x24;              /* TNG3 */
    case 0x38: return grd ? 0x24 : 0x18; /* G4 */
    case 0x39: return 0x1C;              /* NG4 */
    case 0x3C: return 0x24;              /* TG4 */
    case 0x3D: return 0x2C;              /* TNG4 */
    default: return 0;
    }
}

/* Real: objp->tmd = object n's entry of the object table at tmd_base (attribute, coord2 and id are the caller's).
 * Then the object's primitives are grouped into runs of the same mode and flag, and each run's first primitive gets
 * the run's length in its first halfword (GsSortObject4 hands a run to one handler call). A mode LIBGS does not draw
 * is traced and not stepped over, as the PS1 prints it and stays on it. */
void GsLinkObject4(uintptr_t tmd_base, GsDOBJ2 *objp, s32 n) {
    u32 *entry = (u32 *)(tmd_base + (uintptr_t)((intptr_t)n * 28));
    u8 *p, *run;
    u32 count, i, len = 0, prev_mode = 0, prev_flag = 0;

    PSYQ_TRACE("GsLinkObject4 %u obj %u n %d", PSYQ_PTR(tmd_base), PSYQ_PTR(objp), n);
    objp->tmd = entry;
    p = run = psyq_gs_tmd(entry, entry[4]);
    count = entry[5];
    for (i = 0; i < count; i++, len++) {
        u32 w = psyq_gs_word(p), mode = w >> 24, flag = (w >> 16) & 0xFF, size;

        if (prev_mode != 0 && (mode != prev_mode || flag != prev_flag)) {
            memcpy(run, &(u16){ (u16)len }, 2);
            len = 0;
            run = p;
        }
        prev_mode = mode;
        prev_flag = flag;
        size = psyq_gs_prim_size(mode, flag);
        if (size == 0) {
            PSYQ_TRACE("GsLinkObject4: primitive mode %02x not supported", mode);
        }
        p += size;
    }
    memcpy(run, &(u16){ (u16)len }, 2);
}

typedef PACKET *(*PsyqGsHandler)(void *primtop, SVECTOR *vertop, SVECTOR *nortop, PACKET *pk, s32 n, s32 shift,
                                 GsOT *ot, u32 *scratch);
typedef PACKET *(*PsyqGsHandlerN)(void *primtop, SVECTOR *vertop, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                                  u32 *scratch);

/* Real: draws the object (unless its attribute has GsDOFF): its primitives, run by run (GsLinkObject4), each run
 * through its GsFCALL4 entry, into the packet area (GsSetWorkBase) and otp. The entry is chosen by the primitive's
 * mode (semi-transparency aside), its GRD flag for the F and G types, GsDIV (bits 9-11: != 0 also puts the
 * subdivision and the screen's size in scratch[0..2]) and the lighting: 2 with GsLOFF, else 1 (fog) when the
 * lighting mode's bit 0 is set (the attribute's with GsLLMOD, GsSetLightMode's without), else 0. A handler the
 * program did not store stops the run with port_unimplemented (the PS1 jumps to 0). A mode LIBGS does not draw, or a
 * run of length 0 (an object GsLinkObject4 has not seen), ends the object with a trace line (the PS1 prints and loops
 * on it for ever). */
void GsSortObject4(GsDOBJ2 *objp, GsOT *otp, s32 shift, u32 *scratch) {
    u32 attr = objp->attribute;
    u32 *entry;
    SVECTOR *vert, *norm;
    u8 *prim;
    u32 nprim;
    int div, light;
    _GsFCALL *t = &GsFCALL4;

    if ((s32)attr < 0) {
        return;
    }
    psyq_gs.material = attr & 7;
    psyq_gs.lmode = (attr >> 3) & 3;
    psyq_gs.llmod = (attr >> 5) & 1;
    psyq_gs.loff = (attr >> 6) & 1;
    psyq_gs.div = (attr >> 9) & 7;
    psyq_gs.abe = (attr >> 30) & 1;
    div = 0;
    if (psyq_gs.div != 0) {
        scratch[0] = psyq_gs.div;
        scratch[1] = psyq_gs.w;
        scratch[2] = psyq_gs.h;
        div = 1;
    }
    if (psyq_gs.loff == 1) {
        light = 2;
    } else if (psyq_gs.llmod != 0) {
        light = (psyq_gs.lmode & 1) != 0;
    } else {
        light = (psyq_gs.light_mode & 1) != 0;
    }
    entry = objp->tmd;
    vert = psyq_gs_tmd(entry, entry[0]);
    norm = psyq_gs_tmd(entry, entry[2]);
    prim = psyq_gs_tmd(entry, entry[4]);
    nprim = entry[5];
    while (nprim != 0) {
        u32 w = psyq_gs_word(prim), mode = w >> 24, flag = (w >> 16) & 0xFF, n = w & 0xFFFF;
        int grd = (flag & 4) != 0;
        PACKET *(*h)() = NULL;
        int with_normals = 1;
        u32 size = psyq_gs_prim_size(mode, flag);

        switch (mode & 0xFD) {
        case 0x20: h = grd ? t->f3g[light] : t->f3[div][light]; break;
        case 0x21: h = t->nf3[div]; with_normals = 0; break;
        case 0x24: h = t->tf3[div][light]; break;
        case 0x25: h = t->ntf3[div]; with_normals = 0; break;
        case 0x28: h = grd ? t->f4g[light] : t->f4[div][light]; break;
        case 0x29: h = t->nf4[div]; with_normals = 0; break;
        case 0x2C: h = t->tf4[div][light]; break;
        case 0x2D: h = t->ntf4[div]; with_normals = 0; break;
        case 0x30: h = grd ? t->g3g[light] : t->g3[div][light]; break;
        case 0x31: h = t->ng3[div]; with_normals = 0; break;
        case 0x34: h = t->tg3[div][light]; break;
        case 0x35: h = t->ntg3[div]; with_normals = 0; break;
        case 0x38: h = grd ? t->g4g[light] : t->g4[div][light]; break;
        case 0x39: h = t->ng4[div]; with_normals = 0; break;
        case 0x3C: h = t->tg4[div][light]; break;
        case 0x3D: h = t->ntg4[div]; with_normals = 0; break;
        default: break;
        }
        if (size == 0 || n == 0) {
            PSYQ_TRACE("GsSortObject4: %s %x %x, object left", size == 0 ? "non supported code" : "empty run", mode,
                       PSYQ_PTR(prim));
            return;
        }
        if (h == NULL) {
            PSYQ_TRACE("GsSortObject4: no GsFCALL4 handler for mode %02x flag %02x div %d light %d", mode, flag, div,
                       light);
            port_unimplemented("GsSortObject4: a GsFCALL4 entry the program did not store");
            return;
        }
        if (with_normals) {
            GsOUT_PACKET_P = ((PsyqGsHandler)h)(prim, vert, norm, GsOUT_PACKET_P, (s32)n, shift, otp, scratch);
        } else {
            GsOUT_PACKET_P = ((PsyqGsHandlerN)h)(prim, vert, GsOUT_PACKET_P, (s32)n, shift, otp, scratch);
        }
        nprim -= n;
        prim += n * size;
    }
}

/* ---- the handlers' GTE steps ---- */

#define GS_RTPS 0x00180001u  /* sf 1 */
#define GS_RTPT 0x00280030u  /* sf 1 */
#define GS_NCLIP 0x01400006u
#define GS_AVSZ3 0x0158002Du
#define GS_AVSZ4 0x0168002Eu
#define GS_NCCS 0x0108041Bu  /* sf 1, lm 1 */
#define GS_NCCT 0x0118043Fu  /* sf 1, lm 1 */

/* Vn = the SVECTOR at base[i] (lwc2 of its two words). */
static void gs_lv(int n, const SVECTOR *base, u32 i) {
    const u8 *p = (const u8 *)&base[i];

    psyq_gte_mtc2(2 * n, psyq_gs_word(p));
    psyq_gte_mtc2(2 * n + 1, psyq_gs_word(p + 4));
}

/* lwc2 of the primitive's word at byte `off` into data register reg. */
static void gs_lw(int reg, const void *prim, int off) {
    psyq_gte_mtc2(reg, psyq_gs_word((const u8 *)prim + off));
}

/* FLAG's error bit (31) after a perspective transform: the primitive is skipped. */
static int gs_flag_error(void) {
    return (s32)psyq_gte_cfc2(31) < 0;
}

static s32 gs_mac0(void) {
    return (s32)psyq_gte_mfc2(24);
}

static u32 gs_otz(void) {
    return psyq_gte_mfc2(7);
}

/* A packet word copied from the primitive (the uv/clut/tpage words). */
static void gs_copy(PACKET *pk, int to, const void *prim, int from) {
    psyq_gs_put_word(pk + to, psyq_gs_word((const u8 *)prim + from));
}

/* The colour word of a lit or plain F/G primitive: its own (r, g, b, mode as the GPU code) with GsALON's bit. */
static u32 gs_col(const void *prim) {
    return psyq_gs_word((const u8 *)prim + 4) | psyq_gs.abe << 25;
}

/* A textured primitive's code: mode (sign-extended, as lb reads it) with GsALON's bit, in the top byte. */
static u32 gs_tcode(const void *prim) {
    return ((u32)(s32)(s8)((const u8 *)prim)[3] | psyq_gs.abe << 1) << 24;
}

/* An N textured primitive's first colour: the word at off's r, g, b, the code without its bit 0 (texture brightness
 * on: the colour modulates the texture). */
static u32 gs_tncol(const void *prim, int off) {
    u32 c = (u32)(s32)(s8)((const u8 *)prim)[3] | psyq_gs.abe << 1;

    return (psyq_gs_word((const u8 *)prim + off) & 0xFFFFFF) | (c >> 1) << 25;
}

/* An NL Gouraud textured primitive drawn flat and raw (code 0x25 / 0x2D, the mode's semi-transparency bit). */
static u32 gs_tglcode(const void *prim, u32 base) {
    return base | (((u32)(s32)(s8)((const u8 *)prim)[3] & 2) | psyq_gs.abe << 1) << 24;
}

/* The packet's link: its tag takes the ordering-table entry's link and `len`, the entry links to it. The entry is
 * org[((otz - offset) >> shift) & 0xFFFF]; its top byte is kept. */
static void gs_link(GsOT *ot, u32 otz, s32 shift, PACKET *pk, u32 len) {
    u32 *e = (u32 *)ot->org + (((otz - ot->offset) >> (shift & 31)) & 0xFFFF);
    u32 old = *e;

    psyq_gs_put_word(pk, len << 24 | (old & 0xFFFFFF));
    *e = (old & 0xFF000000u) | (port_ptr_to_u32(pk) & 0xFFFFFF);
}

/* The vertex cache: SXYn stored (swc2) into word k, then the cache's words into the packet at the given offsets. */
static void gs_xy(u32 xy[4], int k, int reg) {
    psyq_gte_swc2_(&xy[k], reg);
}

static void gs_put_xy(PACKET *pk, const u32 xy[4], int n, int o0, int o1, int o2, int o3) {
    psyq_gs_put_word(pk + o0, xy[0]);
    psyq_gs_put_word(pk + o1, xy[1]);
    psyq_gs_put_word(pk + o2, xy[2]);
    if (n == 4) {
        psyq_gs_put_word(pk + o3, xy[3]);
    }
}

/* ---- the fast handlers: triangles ---- */

/* F3, lit: POLY_F3, NCCS of n0 over the primitive's colour. */
PACKET *GsTMDfastF3L(TMD_P_F3 *op, SVECTOR *vp, SVECTOR *np, PACKET *pk, s32 n, s32 shift, GsOT *ot, u32 *scratch) {
    u32 xy[4];

    (void)scratch;
    for (; n != 0; n--, op++) {
        u32 otz;

        gs_lv(0, vp, op->v0);
        gs_lv(1, vp, op->v1);
        gs_lv(2, vp, op->v2);
        psyq_gte_cmd(GS_RTPT);
        psyq_gte_mtc2(6, gs_col(op));
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_NCLIP);
        if (gs_mac0() <= 0) {
            continue;
        }
        psyq_gte_cmd(GS_AVSZ3);
        gs_lv(0, np, op->n0);
        otz = gs_otz();
        psyq_gte_cmd(GS_NCCS);
        gs_xy(xy, 0, 12);
        gs_xy(xy, 1, 13);
        gs_xy(xy, 2, 14);
        gs_put_xy(pk, xy, 3, 0x8, 0xC, 0x10, 0);
        psyq_gte_swc2_(pk + 4, 22);
        gs_link(ot, otz, shift, pk, 4);
        pk += 0x14;
    }
    return pk;
}

/* G3, lit: POLY_G3, NCCT of n0..n2. */
PACKET *GsTMDfastG3L(TMD_P_G3 *op, SVECTOR *vp, SVECTOR *np, PACKET *pk, s32 n, s32 shift, GsOT *ot, u32 *scratch) {
    u32 xy[4];

    (void)scratch;
    for (; n != 0; n--, op++) {
        u32 otz;

        gs_lv(0, vp, op->v0);
        gs_lv(1, vp, op->v1);
        gs_lv(2, vp, op->v2);
        psyq_gte_cmd(GS_RTPT);
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_NCLIP);
        psyq_gte_mtc2(6, gs_col(op));
        if (gs_mac0() <= 0) {
            continue;
        }
        psyq_gte_cmd(GS_AVSZ3);
        gs_lv(0, np, op->n0);
        gs_lv(1, np, op->n1);
        gs_lv(2, np, op->n2);
        otz = gs_otz();
        psyq_gte_cmd(GS_NCCT);
        gs_xy(xy, 0, 12);
        gs_xy(xy, 1, 13);
        gs_xy(xy, 2, 14);
        gs_put_xy(pk, xy, 3, 0x8, 0x10, 0x18, 0);
        psyq_gte_swc2_(pk + 0x4, 20);
        psyq_gte_swc2_(pk + 0xC, 21);
        psyq_gte_swc2_(pk + 0x14, 22);
        gs_link(ot, otz, shift, pk, 6);
        pk += 0x1C;
    }
    return pk;
}

/* TF3, lit: POLY_FT3, NCCS of n0 over grey 0x808080. The texture words are copied for every primitive. */
PACKET *GsTMDfastTF3L(TMD_P_TF3 *op, SVECTOR *vp, SVECTOR *np, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                      u32 *scratch) {
    u32 xy[4];

    (void)scratch;
    for (; n != 0; n--, op++) {
        u32 otz;

        gs_lv(0, vp, op->v0);
        gs_lv(1, vp, op->v1);
        gs_lv(2, vp, op->v2);
        psyq_gte_cmd(GS_RTPT);
        psyq_gte_mtc2(6, 0x808080 | gs_tcode(op));
        gs_copy(pk, 0xC, op, 4);
        gs_copy(pk, 0x14, op, 8);
        gs_copy(pk, 0x1C, op, 0xC);
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_NCLIP);
        if (gs_mac0() <= 0) {
            continue;
        }
        psyq_gte_cmd(GS_AVSZ3);
        gs_lv(0, np, op->n0);
        otz = gs_otz();
        psyq_gte_cmd(GS_NCCS);
        gs_xy(xy, 0, 12);
        gs_xy(xy, 1, 13);
        gs_xy(xy, 2, 14);
        gs_put_xy(pk, xy, 3, 0x8, 0x10, 0x18, 0);
        psyq_gte_swc2_(pk + 4, 22);
        gs_link(ot, otz, shift, pk, 7);
        pk += 0x20;
    }
    return pk;
}

/* TF3, unlit: POLY_FT3 in grey 0x808080. */
PACKET *GsTMDfastTF3NL(TMD_P_TF3 *op, SVECTOR *vp, SVECTOR *np, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                       u32 *scratch) {
    u32 xy[4];

    (void)np;
    (void)scratch;
    for (; n != 0; n--, op++) {
        gs_lv(0, vp, op->v0);
        gs_lv(1, vp, op->v1);
        gs_lv(2, vp, op->v2);
        psyq_gte_cmd(GS_RTPT);
        gs_copy(pk, 0xC, op, 4);
        gs_copy(pk, 0x14, op, 8);
        gs_copy(pk, 0x1C, op, 0xC);
        psyq_gs_put_word(pk + 4, 0x808080 | gs_tcode(op));
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_NCLIP);
        if (gs_mac0() <= 0) {
            continue;
        }
        psyq_gte_cmd(GS_AVSZ3);
        gs_xy(xy, 0, 12);
        gs_xy(xy, 1, 13);
        gs_xy(xy, 2, 14);
        gs_put_xy(pk, xy, 3, 0x8, 0x10, 0x18, 0);
        gs_link(ot, gs_otz(), shift, pk, 7);
        pk += 0x20;
    }
    return pk;
}

/* TG3, lit: POLY_GT3, NCCT of n0..n2 over grey. */
PACKET *GsTMDfastTG3L(TMD_P_TG3 *op, SVECTOR *vp, SVECTOR *np, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                      u32 *scratch) {
    u32 xy[4];

    (void)scratch;
    for (; n != 0; n--, op++) {
        u32 otz;

        gs_lv(0, vp, op->v0);
        gs_lv(1, vp, op->v1);
        gs_lv(2, vp, op->v2);
        psyq_gte_cmd(GS_RTPT);
        psyq_gte_mtc2(6, 0x808080 | gs_tcode(op));
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_NCLIP);
        if (gs_mac0() <= 0) {
            continue;
        }
        psyq_gte_cmd(GS_AVSZ3);
        gs_lv(0, np, op->n0);
        gs_lv(1, np, op->n1);
        gs_lv(2, np, op->n2);
        otz = gs_otz();
        psyq_gte_cmd(GS_NCCT);
        gs_copy(pk, 0xC, op, 4);
        gs_copy(pk, 0x18, op, 8);
        gs_copy(pk, 0x24, op, 0xC);
        gs_xy(xy, 0, 12);
        gs_xy(xy, 1, 13);
        gs_xy(xy, 2, 14);
        gs_put_xy(pk, xy, 3, 0x8, 0x14, 0x20, 0);
        psyq_gte_swc2_(pk + 0x4, 20);
        psyq_gte_swc2_(pk + 0x10, 21);
        psyq_gte_swc2_(pk + 0x1C, 22);
        gs_link(ot, otz, shift, pk, 9);
        pk += 0x28;
    }
    return pk;
}

/* TG3, unlit: POLY_FT3, raw texture (code 0x25). The texture words pass through RGB0..2 (lwc2, swc2). */
PACKET *GsTMDfastTG3NL(TMD_P_TG3 *op, SVECTOR *vp, SVECTOR *np, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                       u32 *scratch) {
    u32 xy[4];

    (void)np;
    (void)scratch;
    for (; n != 0; n--, op++) {
        u32 otz;

        gs_lv(0, vp, op->v0);
        gs_lv(1, vp, op->v1);
        gs_lv(2, vp, op->v2);
        psyq_gte_cmd(GS_RTPT);
        gs_lw(20, op, 4);
        gs_lw(21, op, 8);
        gs_lw(22, op, 0xC);
        psyq_gs_put_word(pk + 4, gs_tglcode(op, 0x25808080u));
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_NCLIP);
        if (gs_mac0() <= 0) {
            continue;
        }
        psyq_gte_cmd(GS_AVSZ3);
        otz = gs_otz();
        psyq_gte_swc2_(pk + 0xC, 20);
        psyq_gte_swc2_(pk + 0x14, 21);
        psyq_gte_swc2_(pk + 0x1C, 22);
        gs_xy(xy, 0, 12);
        gs_xy(xy, 1, 13);
        gs_xy(xy, 2, 14);
        gs_put_xy(pk, xy, 3, 0x8, 0x10, 0x18, 0);
        gs_link(ot, otz, shift, pk, 7);
        pk += 0x20;
    }
    return pk;
}

/* TNF3: POLY_FT3 in the primitive's colour (texture brightness on). */
PACKET *GsTMDfastTNF3(TMD_P_TNF3 *op, SVECTOR *vp, PACKET *pk, s32 n, s32 shift, GsOT *ot, u32 *scratch) {
    u32 xy[4];

    (void)scratch;
    for (; n != 0; n--, op++) {
        u32 otz;

        gs_lv(0, vp, op->v0);
        gs_lv(1, vp, op->v1);
        gs_lv(2, vp, op->v2);
        psyq_gte_cmd(GS_RTPT);
        psyq_gte_mtc2(22, gs_tncol(op, 0x10));
        gs_copy(pk, 0xC, op, 4);
        gs_copy(pk, 0x14, op, 8);
        gs_copy(pk, 0x1C, op, 0xC);
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_NCLIP);
        if (gs_mac0() <= 0) {
            continue;
        }
        psyq_gte_cmd(GS_AVSZ3);
        otz = gs_otz();
        psyq_gte_swc2_(pk + 4, 22);
        gs_xy(xy, 0, 12);
        gs_xy(xy, 1, 13);
        gs_xy(xy, 2, 14);
        gs_put_xy(pk, xy, 3, 0x8, 0x10, 0x18, 0);
        gs_link(ot, otz, shift, pk, 7);
        pk += 0x20;
    }
    return pk;
}

/* TNG3: POLY_GT3 in the primitive's three colours. */
PACKET *GsTMDfastTNG3(TMD_P_TNG3 *op, SVECTOR *vp, PACKET *pk, s32 n, s32 shift, GsOT *ot, u32 *scratch) {
    u32 xy[4];

    (void)scratch;
    for (; n != 0; n--, op++) {
        u32 otz;

        gs_lv(0, vp, op->v0);
        gs_lv(1, vp, op->v1);
        gs_lv(2, vp, op->v2);
        psyq_gte_cmd(GS_RTPT);
        psyq_gte_mtc2(20, gs_tncol(op, 0x10));
        gs_lw(21, op, 0x14);
        gs_lw(22, op, 0x18);
        gs_copy(pk, 0xC, op, 4);
        gs_copy(pk, 0x18, op, 8);
        gs_copy(pk, 0x24, op, 0xC);
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_NCLIP);
        if (gs_mac0() <= 0) {
            continue;
        }
        psyq_gte_cmd(GS_AVSZ3);
        otz = gs_otz();
        gs_xy(xy, 0, 12);
        gs_xy(xy, 1, 13);
        gs_xy(xy, 2, 14);
        gs_put_xy(pk, xy, 3, 0x8, 0x14, 0x20, 0);
        psyq_gte_swc2_(pk + 0x4, 20);
        psyq_gte_swc2_(pk + 0x10, 21);
        psyq_gte_swc2_(pk + 0x1C, 22);
        gs_link(ot, otz, shift, pk, 9);
        pk += 0x28;
    }
    return pk;
}

/* ---- the fast handlers: quads (the back-face test on v0..v2, then RTPS of v3) ---- */

/* F4, lit: POLY_F4, NCCS of n0. */
PACKET *GsTMDfastF4L(TMD_P_F4 *op, SVECTOR *vp, SVECTOR *np, PACKET *pk, s32 n, s32 shift, GsOT *ot, u32 *scratch) {
    u32 xy[4];

    (void)scratch;
    for (; n != 0; n--, op++) {
        u32 otz;

        gs_lv(0, vp, op->v0);
        gs_lv(1, vp, op->v1);
        gs_lv(2, vp, op->v2);
        psyq_gte_cmd(GS_RTPT);
        psyq_gte_mtc2(6, gs_col(op));
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_NCLIP);
        gs_lv(0, vp, op->v3);
        if (gs_mac0() <= 0) {
            continue;
        }
        gs_xy(xy, 0, 12);
        gs_xy(xy, 1, 13);
        gs_xy(xy, 2, 14);
        psyq_gte_cmd(GS_RTPS);
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_AVSZ4);
        gs_lv(0, np, op->n0);
        otz = gs_otz();
        psyq_gte_cmd(GS_NCCS);
        gs_xy(xy, 3, 14);
        gs_put_xy(pk, xy, 4, 0x8, 0xC, 0x10, 0x14);
        psyq_gte_swc2_(pk + 4, 22);
        gs_link(ot, otz, shift, pk, 5);
        pk += 0x18;
    }
    return pk;
}

/* F4, unlit: POLY_F4 in the primitive's colour. */
PACKET *GsTMDfastF4NL(TMD_P_F4 *op, SVECTOR *vp, SVECTOR *np, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                      u32 *scratch) {
    u32 xy[4];

    (void)np;
    (void)scratch;
    for (; n != 0; n--, op++) {
        gs_lv(0, vp, op->v0);
        gs_lv(1, vp, op->v1);
        gs_lv(2, vp, op->v2);
        psyq_gte_cmd(GS_RTPT);
        psyq_gte_mtc2(22, gs_col(op));
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_NCLIP);
        gs_lv(0, vp, op->v3);
        if (gs_mac0() <= 0) {
            continue;
        }
        psyq_gte_swc2_(pk + 4, 22);
        gs_xy(xy, 0, 12);
        gs_xy(xy, 1, 13);
        gs_xy(xy, 2, 14);
        psyq_gte_cmd(GS_RTPS);
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_AVSZ4);
        gs_xy(xy, 3, 14);
        gs_put_xy(pk, xy, 4, 0x8, 0xC, 0x10, 0x14);
        gs_link(ot, gs_otz(), shift, pk, 5);
        pk += 0x18;
    }
    return pk;
}

/* NF4: POLY_F4 in the primitive's colour (no normals). */
PACKET *GsTMDfastNF4(TMD_P_NF4 *op, SVECTOR *vp, PACKET *pk, s32 n, s32 shift, GsOT *ot, u32 *scratch) {
    u32 xy[4];

    (void)scratch;
    for (; n != 0; n--, op++) {
        gs_lv(0, vp, op->v0);
        gs_lv(1, vp, op->v1);
        gs_lv(2, vp, op->v2);
        psyq_gte_cmd(GS_RTPT);
        psyq_gte_mtc2(22, gs_col(op));
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_NCLIP);
        gs_lv(0, vp, op->v3);
        if (gs_mac0() <= 0) {
            continue;
        }
        psyq_gte_swc2_(pk + 4, 22);
        gs_xy(xy, 0, 12);
        gs_xy(xy, 1, 13);
        gs_xy(xy, 2, 14);
        psyq_gte_cmd(GS_RTPS);
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_AVSZ4);
        gs_xy(xy, 3, 14);
        gs_put_xy(pk, xy, 4, 0x8, 0xC, 0x10, 0x14);
        gs_link(ot, gs_otz(), shift, pk, 5);
        pk += 0x18;
    }
    return pk;
}

/* G4, lit: POLY_G4, NCCS of n3 then NCCT of n0..n2. */
PACKET *GsTMDfastG4L(TMD_P_G4 *op, SVECTOR *vp, SVECTOR *np, PACKET *pk, s32 n, s32 shift, GsOT *ot, u32 *scratch) {
    u32 xy[4];

    (void)scratch;
    for (; n != 0; n--, op++) {
        u32 otz;

        gs_lv(0, vp, op->v0);
        gs_lv(1, vp, op->v1);
        gs_lv(2, vp, op->v2);
        psyq_gte_cmd(GS_RTPT);
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_NCLIP);
        gs_lv(0, vp, op->v3);
        if (gs_mac0() <= 0) {
            continue;
        }
        gs_xy(xy, 0, 12);
        gs_xy(xy, 1, 13);
        gs_xy(xy, 2, 14);
        psyq_gte_cmd(GS_RTPS);
        psyq_gte_mtc2(6, gs_col(op));
        gs_lv(1, np, op->n1);
        gs_lv(2, np, op->n2);
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_AVSZ4);
        gs_lv(0, np, op->n3);
        otz = gs_otz();
        psyq_gte_cmd(GS_NCCS);
        psyq_gte_swc2_(pk + 0x1C, 22);
        gs_lv(0, np, op->n0);
        psyq_gte_cmd(GS_NCCT);
        gs_xy(xy, 3, 14);
        gs_put_xy(pk, xy, 4, 0x8, 0x10, 0x18, 0x20);
        psyq_gte_swc2_(pk + 0x4, 20);
        psyq_gte_swc2_(pk + 0xC, 21);
        psyq_gte_swc2_(pk + 0x14, 22);
        gs_link(ot, otz, shift, pk, 8);
        pk += 0x24;
    }
    return pk;
}

/* TF4, lit: POLY_FT4, NCCS of n0 over grey. */
PACKET *GsTMDfastTF4L(TMD_P_TF4 *op, SVECTOR *vp, SVECTOR *np, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                      u32 *scratch) {
    u32 xy[4];

    (void)scratch;
    for (; n != 0; n--, op++) {
        u32 otz;

        gs_lv(0, vp, op->v0);
        gs_lv(1, vp, op->v1);
        gs_lv(2, vp, op->v2);
        psyq_gte_cmd(GS_RTPT);
        psyq_gte_mtc2(6, 0x808080 | gs_tcode(op));
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_NCLIP);
        gs_lv(0, vp, op->v3);
        if (gs_mac0() <= 0) {
            continue;
        }
        gs_xy(xy, 0, 12);
        gs_xy(xy, 1, 13);
        gs_xy(xy, 2, 14);
        psyq_gte_cmd(GS_RTPS);
        gs_copy(pk, 0xC, op, 4);
        gs_copy(pk, 0x14, op, 8);
        gs_copy(pk, 0x1C, op, 0xC);
        gs_copy(pk, 0x24, op, 0x10);
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_AVSZ4);
        gs_lv(0, np, op->n0);
        otz = gs_otz();
        psyq_gte_cmd(GS_NCCS);
        gs_xy(xy, 3, 14);
        gs_put_xy(pk, xy, 4, 0x8, 0x10, 0x18, 0x20);
        psyq_gte_swc2_(pk + 4, 22);
        gs_link(ot, otz, shift, pk, 9);
        pk += 0x28;
    }
    return pk;
}

/* TF4, unlit: POLY_FT4 in grey. */
PACKET *GsTMDfastTF4NL(TMD_P_TF4 *op, SVECTOR *vp, SVECTOR *np, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                       u32 *scratch) {
    u32 xy[4];

    (void)np;
    (void)scratch;
    for (; n != 0; n--, op++) {
        gs_lv(0, vp, op->v0);
        gs_lv(1, vp, op->v1);
        gs_lv(2, vp, op->v2);
        psyq_gte_cmd(GS_RTPT);
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_NCLIP);
        gs_lv(0, vp, op->v3);
        if (gs_mac0() <= 0) {
            continue;
        }
        gs_xy(xy, 0, 12);
        gs_xy(xy, 1, 13);
        gs_xy(xy, 2, 14);
        psyq_gte_cmd(GS_RTPS);
        gs_copy(pk, 0xC, op, 4);
        gs_copy(pk, 0x14, op, 8);
        gs_copy(pk, 0x1C, op, 0xC);
        gs_copy(pk, 0x24, op, 0x10);
        psyq_gs_put_word(pk + 4, 0x808080 | gs_tcode(op));
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_AVSZ4);
        gs_xy(xy, 3, 14);
        gs_put_xy(pk, xy, 4, 0x8, 0x10, 0x18, 0x20);
        gs_link(ot, gs_otz(), shift, pk, 9);
        pk += 0x28;
    }
    return pk;
}

/* TG4, lit: POLY_GT4, NCCS of n3 then NCCT of n0..n2, over grey. */
PACKET *GsTMDfastTG4L(TMD_P_TG4 *op, SVECTOR *vp, SVECTOR *np, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                      u32 *scratch) {
    u32 xy[4];

    (void)scratch;
    for (; n != 0; n--, op++) {
        u32 otz;

        gs_lv(0, vp, op->v0);
        gs_lv(1, vp, op->v1);
        gs_lv(2, vp, op->v2);
        psyq_gte_cmd(GS_RTPT);
        psyq_gte_mtc2(6, 0x808080 | gs_tcode(op));
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_NCLIP);
        gs_lv(0, vp, op->v3);
        if (gs_mac0() <= 0) {
            continue;
        }
        gs_xy(xy, 0, 12);
        gs_xy(xy, 1, 13);
        gs_xy(xy, 2, 14);
        psyq_gte_cmd(GS_RTPS);
        gs_lv(1, np, op->n1);
        gs_lv(2, np, op->n2);
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_AVSZ4);
        gs_lv(0, np, op->n3);
        otz = gs_otz();
        psyq_gte_cmd(GS_NCCS);
        gs_copy(pk, 0xC, op, 4);
        gs_copy(pk, 0x18, op, 8);
        gs_copy(pk, 0x24, op, 0xC);
        gs_copy(pk, 0x30, op, 0x10);
        psyq_gte_swc2_(pk + 0x28, 22);
        gs_lv(0, np, op->n0);
        psyq_gte_cmd(GS_NCCT);
        gs_xy(xy, 3, 14);
        gs_put_xy(pk, xy, 4, 0x8, 0x14, 0x20, 0x2C);
        psyq_gte_swc2_(pk + 0x4, 20);
        psyq_gte_swc2_(pk + 0x10, 21);
        psyq_gte_swc2_(pk + 0x1C, 22);
        gs_link(ot, otz, shift, pk, 12);
        pk += 0x34;
    }
    return pk;
}

/* TG4, unlit: POLY_FT4, raw texture (code 0x2D). */
PACKET *GsTMDfastTG4NL(TMD_P_TG4 *op, SVECTOR *vp, SVECTOR *np, PACKET *pk, s32 n, s32 shift, GsOT *ot,
                       u32 *scratch) {
    u32 xy[4];

    (void)np;
    (void)scratch;
    for (; n != 0; n--, op++) {
        u32 otz;

        gs_lv(0, vp, op->v0);
        gs_lv(1, vp, op->v1);
        gs_lv(2, vp, op->v2);
        psyq_gte_cmd(GS_RTPT);
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_NCLIP);
        gs_lv(0, vp, op->v3);
        if (gs_mac0() <= 0) {
            continue;
        }
        gs_xy(xy, 0, 12);
        gs_xy(xy, 1, 13);
        gs_xy(xy, 2, 14);
        psyq_gte_cmd(GS_RTPS);
        psyq_gs_put_word(pk + 4, gs_tglcode(op, 0x2D808080u));
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_AVSZ4);
        gs_copy(pk, 0xC, op, 4);
        gs_copy(pk, 0x14, op, 8);
        gs_copy(pk, 0x1C, op, 0xC);
        otz = gs_otz();
        gs_copy(pk, 0x24, op, 0x10);
        gs_xy(xy, 3, 14);
        gs_put_xy(pk, xy, 4, 0x8, 0x10, 0x18, 0x20);
        gs_link(ot, otz, shift, pk, 9);
        pk += 0x28;
    }
    return pk;
}

/* TNF4: POLY_FT4 in the primitive's colour. */
PACKET *GsTMDfastTNF4(TMD_P_TNF4 *op, SVECTOR *vp, PACKET *pk, s32 n, s32 shift, GsOT *ot, u32 *scratch) {
    u32 xy[4];

    (void)scratch;
    for (; n != 0; n--, op++) {
        gs_lv(0, vp, op->v0);
        gs_lv(1, vp, op->v1);
        gs_lv(2, vp, op->v2);
        psyq_gte_cmd(GS_RTPT);
        psyq_gs_put_word(pk + 4, gs_tncol(op, 0x14));
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_NCLIP);
        gs_lv(0, vp, op->v3);
        if (gs_mac0() <= 0) {
            continue;
        }
        gs_xy(xy, 0, 12);
        gs_xy(xy, 1, 13);
        gs_xy(xy, 2, 14);
        psyq_gte_cmd(GS_RTPS);
        gs_copy(pk, 0xC, op, 4);
        gs_copy(pk, 0x14, op, 8);
        gs_copy(pk, 0x1C, op, 0xC);
        gs_copy(pk, 0x24, op, 0x10);
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_AVSZ4);
        gs_xy(xy, 3, 14);
        gs_put_xy(pk, xy, 4, 0x8, 0x10, 0x18, 0x20);
        gs_link(ot, gs_otz(), shift, pk, 9);
        pk += 0x28;
    }
    return pk;
}

/* TNG4: POLY_GT4 in the primitive's four colours (the fourth through RGBC). */
PACKET *GsTMDfastTNG4(TMD_P_TNG4 *op, SVECTOR *vp, PACKET *pk, s32 n, s32 shift, GsOT *ot, u32 *scratch) {
    u32 xy[4];

    (void)scratch;
    for (; n != 0; n--, op++) {
        u32 otz;

        gs_lv(0, vp, op->v0);
        gs_lv(1, vp, op->v1);
        gs_lv(2, vp, op->v2);
        psyq_gte_cmd(GS_RTPT);
        psyq_gte_mtc2(20, gs_tncol(op, 0x14));
        gs_lw(21, op, 0x18);
        gs_lw(22, op, 0x1C);
        gs_lw(6, op, 0x20);
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_NCLIP);
        gs_lv(0, vp, op->v3);
        if (gs_mac0() <= 0) {
            continue;
        }
        gs_xy(xy, 0, 12);
        gs_xy(xy, 1, 13);
        gs_xy(xy, 2, 14);
        psyq_gte_cmd(GS_RTPS);
        if (gs_flag_error()) {
            continue;
        }
        psyq_gte_cmd(GS_AVSZ4);
        gs_copy(pk, 0xC, op, 4);
        gs_copy(pk, 0x18, op, 8);
        gs_copy(pk, 0x24, op, 0xC);
        gs_copy(pk, 0x30, op, 0x10);
        otz = gs_otz();
        gs_xy(xy, 3, 14);
        gs_put_xy(pk, xy, 4, 0x8, 0x14, 0x20, 0x2C);
        psyq_gte_swc2_(pk + 0x4, 20);
        psyq_gte_swc2_(pk + 0x10, 21);
        psyq_gte_swc2_(pk + 0x1C, 22);
        psyq_gte_swc2_(pk + 0x28, 6);
        gs_link(ot, otz, shift, pk, 12);
        pk += 0x34;
    }
    return pk;
}

/* ---- the subdividing handlers (GsDIV): not in the shim yet ---- */

#define PSYQ_GS_DIV(name, type)                                                                                    \
    PACKET *name(type *op, SVECTOR *vp, SVECTOR *np, PACKET *pk, s32 n, s32 shift, GsOT *ot, u32 *scratch) {     \
        (void)op, (void)vp, (void)np, (void)n, (void)shift, (void)ot, (void)scratch;                               \
        port_unimplemented(#name);                                                                                 \
        return pk;                                                                                                 \
    }
#define PSYQ_GS_DIV_N(name, type)                                                                                  \
    PACKET *name(type *op, SVECTOR *vp, PACKET *pk, s32 n, s32 shift, GsOT *ot, u32 *scratch) {                   \
        (void)op, (void)vp, (void)n, (void)shift, (void)ot, (void)scratch;                                         \
        port_unimplemented(#name);                                                                                 \
        return pk;                                                                                                 \
    }

PSYQ_GS_DIV(GsTMDdivTF3NL, TMD_P_TF3)
PSYQ_GS_DIV(GsTMDdivTG3NL, TMD_P_TG3)
PSYQ_GS_DIV(GsTMDdivTF4L, TMD_P_TF4)
PSYQ_GS_DIV(GsTMDdivTF4NL, TMD_P_TF4)
PSYQ_GS_DIV(GsTMDdivTG4NL, TMD_P_TG4)
PSYQ_GS_DIV_N(GsTMDdivTNF3, TMD_P_TNF3)
PSYQ_GS_DIV_N(GsTMDdivTNG3, TMD_P_TNG3)
PSYQ_GS_DIV_N(GsTMDdivTNF4, TMD_P_TNF4)
PSYQ_GS_DIV_N(GsTMDdivTNG4, TMD_P_TNG4)

/* The primitive layouts (libgs.h) are the TMD format's sizes. */
typedef char psyq_gs_tmd_sizes[(sizeof(TMD_P_F3) == 0x10 && sizeof(TMD_P_G3) == 0x14 && sizeof(TMD_P_F4) == 0x14 &&
                                sizeof(TMD_P_G4) == 0x18 && sizeof(TMD_P_TF3) == 0x18 && sizeof(TMD_P_TG3) == 0x1C &&
                                sizeof(TMD_P_TF4) == 0x20 && sizeof(TMD_P_TG4) == 0x24 && sizeof(TMD_P_NF4) == 0x10 &&
                                sizeof(TMD_P_TNF3) == 0x1C && sizeof(TMD_P_TNG3) == 0x24 &&
                                sizeof(TMD_P_TNF4) == 0x20 && sizeof(TMD_P_TNG4) == 0x2C)
                                   ? 1
                                   : -1];
