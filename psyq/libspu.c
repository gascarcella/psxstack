/* psyq/libspu.c: the LIBSPU calls a game makes itself (the first game makes none; LIBSND's own use of LIBSPU is
 * libsnd_spu.c): SpuSetVoiceAttr, SpuSetCommonAttr, SpuClearReverbWorkArea, over the SPU core's registers
 * (include/psxstack/spu.h; psx-spx "SPU Voice Volume", "SPU ADSR", "SPU Control and Status Register"). Written from the
 * games' use (Digimon Digital Card Battle: every voice's release rate set to 0 before a new track, the opening movie's
 * main and CD volumes and the CD's mix) and LIBSPU's public attribute masks.
 *
 * Each attribute in the mask is written to the register that holds it, voice by voice in voice order: volumes (left,
 * right), pitch, start address, ADSR1 (attack mode and rate, decay rate, sustain level), ADSR2 (sustain mode and
 * rate, release mode and rate), loop address. An ADSR field alone changes only its bits of the register's current
 * value. Volumes are direct (volmode 0); a sweep mode, and NOTE/SAMPLE_NOTE (a pitch from a note), are not modelled:
 * they stop the run (port_unimplemented). The order of LIBSPU's own register writes is not known: this one is ours
 * (no SPU write trace of the second game yet). */
#include "libsnd_internal.h"
#include "psxstack/psyq/libspu.h"

/* LIBSPU's envelope modes (SpuVoiceAttr a_mode, s_mode, r_mode). */
#define SPU_MODE_LINEAR_INC_N 1
#define SPU_MODE_LINEAR_INC_R 2
#define SPU_MODE_LINEAR_DEC_N 3
#define SPU_MODE_LINEAR_DEC_R 4
#define SPU_MODE_EXP_INC_N 5
#define SPU_MODE_EXP_INC_R 6
#define SPU_MODE_EXP_DEC 7

static int spu_mode_exp(s32 mode) {
    return mode == SPU_MODE_EXP_INC_N || mode == SPU_MODE_EXP_INC_R || mode == SPU_MODE_EXP_DEC;
}

static int spu_mode_dec(s32 mode) {
    return mode == SPU_MODE_LINEAR_DEC_N || mode == SPU_MODE_LINEAR_DEC_R || mode == SPU_MODE_EXP_DEC;
}

static u16 spu_bits(u16 reg, u16 mask, u32 value) {
    return (u16)((reg & ~mask) | (value & mask));
}

void SpuSetVoiceAttr(SpuVoiceAttr *arg) {
    u32 m = arg->mask;
    int v;

    PSYQ_TRACE("SpuSetVoiceAttr voices %06X mask %05X", arg->voice & 0xFFFFFF, m);
    if (m & (SPU_VOICE_NOTE | SPU_VOICE_SAMPLE_NOTE)) {
        port_unimplemented("SpuSetVoiceAttr: a pitch from a note (SPU_VOICE_NOTE, SPU_VOICE_SAMPLE_NOTE)");
    }
    if (((m & SPU_VOICE_VOLMODEL) && arg->volmode.left != 0) || ((m & SPU_VOICE_VOLMODER) && arg->volmode.right != 0)) {
        port_unimplemented("SpuSetVoiceAttr: a volume sweep (volmode)");
    }
    for (v = 0; v < SND_VOICES; v++) {
        u32 base = (u32)v * 16;
        u16 adsr1, adsr2;

        if (!(arg->voice & (1u << v))) {
            continue;
        }
        if (m & SPU_VOICE_VOLL) {
            spu_write16(base + 0, (u16)arg->volume.left & 0x7FFF);
        }
        if (m & SPU_VOICE_VOLR) {
            spu_write16(base + 2, (u16)arg->volume.right & 0x7FFF);
        }
        if (m & SPU_VOICE_PITCH) {
            spu_write16(base + 4, arg->pitch);
        }
        if (m & SPU_VOICE_WDSA) {
            spu_write16(base + 6, (u16)(arg->addr >> 3));
        }
        adsr1 = spu_read16(base + 8);
        adsr2 = spu_read16(base + 10);
        if (m & SPU_VOICE_ADSR_ADSR1) {
            adsr1 = arg->adsr1;
        }
        if (m & SPU_VOICE_ADSR_ADSR2) {
            adsr2 = arg->adsr2;
        }
        if (m & SPU_VOICE_ADSR_AMODE) {
            adsr1 = spu_bits(adsr1, 0x8000, spu_mode_exp(arg->a_mode) ? 0x8000u : 0);
        }
        if (m & SPU_VOICE_ADSR_AR) {
            adsr1 = spu_bits(adsr1, 0x7F00, (u32)arg->ar << 8);
        }
        if (m & SPU_VOICE_ADSR_DR) {
            adsr1 = spu_bits(adsr1, 0x00F0, (u32)arg->dr << 4);
        }
        if (m & SPU_VOICE_ADSR_SL) {
            adsr1 = spu_bits(adsr1, 0x000F, arg->sl);
        }
        if (m & SPU_VOICE_ADSR_SMODE) {
            adsr2 = spu_bits(adsr2, 0xC000, (spu_mode_exp(arg->s_mode) ? 0x8000u : 0) |
                                             (spu_mode_dec(arg->s_mode) ? 0x4000u : 0));
        }
        if (m & SPU_VOICE_ADSR_SR) {
            adsr2 = spu_bits(adsr2, 0x1FC0, (u32)arg->sr << 6);
        }
        if (m & SPU_VOICE_ADSR_RMODE) {
            adsr2 = spu_bits(adsr2, 0x0020, spu_mode_exp(arg->r_mode) ? 0x20u : 0);
        }
        if (m & SPU_VOICE_ADSR_RR) {
            adsr2 = spu_bits(adsr2, 0x001F, arg->rr);
        }
        if (m & (SPU_VOICE_ADSR_ADSR1 | SPU_VOICE_ADSR_AMODE | SPU_VOICE_ADSR_AR | SPU_VOICE_ADSR_DR |
                 SPU_VOICE_ADSR_SL)) {
            spu_write16(base + 8, adsr1);
        }
        if (m & (SPU_VOICE_ADSR_ADSR2 | SPU_VOICE_ADSR_SMODE | SPU_VOICE_ADSR_SR | SPU_VOICE_ADSR_RMODE |
                 SPU_VOICE_ADSR_RR)) {
            spu_write16(base + 10, adsr2);
        }
        if (m & SPU_VOICE_LSAX) {
            spu_write16(base + 14, (u16)(arg->loop_addr >> 3));
        }
    }
}

/* The main volume (direct), the CD's and the external input's volumes, and SPUCNT's bits for them: CD audio enable
 * (bit 0), external enable (bit 1), CD reverb (bit 2), external reverb (bit 3). */
void SpuSetCommonAttr(SpuCommonAttr *attr) {
    u32 m = attr->mask;
    u16 cnt;

    PSYQ_TRACE("SpuSetCommonAttr mask %04X", m);
    if (((m & SPU_COMMON_MVOLMODEL) && attr->mvolmode.left != 0) ||
        ((m & SPU_COMMON_MVOLMODER) && attr->mvolmode.right != 0)) {
        port_unimplemented("SpuSetCommonAttr: a main volume sweep (mvolmode)");
    }
    if (m & SPU_COMMON_MVOLL) {
        spu_write16(SND_REG_MVOL_L, (u16)attr->mvol.left & 0x7FFF);
    }
    if (m & SPU_COMMON_MVOLR) {
        spu_write16(SND_REG_MVOL_R, (u16)attr->mvol.right & 0x7FFF);
    }
    if (m & SPU_COMMON_CDVOLL) {
        spu_write16(SND_REG_CDVOL_L, (u16)attr->cd.volume.left);
    }
    if (m & SPU_COMMON_CDVOLR) {
        spu_write16(SND_REG_CDVOL_R, (u16)attr->cd.volume.right);
    }
    if (m & SPU_COMMON_EXTVOLL) {
        spu_write16(SND_REG_EXTVOL_L, (u16)attr->ext.volume.left);
    }
    if (m & SPU_COMMON_EXTVOLR) {
        spu_write16(SND_REG_EXTVOL_R, (u16)attr->ext.volume.right);
    }
    if (m & (SPU_COMMON_CDREV | SPU_COMMON_CDMIX | SPU_COMMON_EXTREV | SPU_COMMON_EXTMIX)) {
        cnt = spu_read16(SND_REG_ATTR);
        if (m & SPU_COMMON_CDMIX) {
            cnt = spu_bits(cnt, 0x0001, attr->cd.mix ? 1u : 0);
        }
        if (m & SPU_COMMON_EXTMIX) {
            cnt = spu_bits(cnt, 0x0002, attr->ext.mix ? 2u : 0);
        }
        if (m & SPU_COMMON_CDREV) {
            cnt = spu_bits(cnt, 0x0004, attr->cd.reverb ? 4u : 0);
        }
        if (m & SPU_COMMON_EXTREV) {
            cnt = spu_bits(cnt, 0x0008, attr->ext.reverb ? 8u : 0);
        }
        spu_write16(SND_REG_ATTR, cnt);
    }
}

/* The work area of reverb type `rev_mode` (0..9, SsUtSetReverbType's) zeroed by DMA: 0, or -1 for another type. */
s32 SpuClearReverbWorkArea(s32 rev_mode) {
    PSYQ_TRACE("SpuClearReverbWorkArea %d", rev_mode);
    return snd_spu_clear_reverb_type(rev_mode);
}
