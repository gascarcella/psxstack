#ifndef PSYQ_LIBSPU_H
#define PSYQ_LIBSPU_H

/* psxstack's declarations of the Psy-Q 4.7 LIBSPU interface, as its games use it: written from the games' use of the
 * API and public documentation, no Sony header (DECISIONS "Psy-Q declarations: the stack's"). The shim implements
 * these (psyq/libspu.c); a game's own recovered declarations must agree with them in ABI (tools/psyq_decls.py). */

#include "psxstack/types.h"

typedef struct SpuVolume {
    s16 left;
    s16 right;
} SpuVolume;

/* SpuSetVoiceAttr: `voice` a bit per voice (0..23), `mask` the attributes to set (SPU_VOICE_*). */
typedef struct SpuVoiceAttr {
    u32 voice;
    u32 mask;
    SpuVolume volume;
    SpuVolume volmode;
    SpuVolume volumex;
    u16 pitch;
    u16 note;
    u16 sample_note;
    s16 envx;
    u32 addr;
    u32 loop_addr;
    s32 a_mode;
    s32 s_mode;
    s32 r_mode;
    u16 ar;
    u16 dr;
    u16 sr;
    u16 rr;
    u16 sl;
    u16 adsr1;
    u16 adsr2;
} SpuVoiceAttr;

typedef struct SpuExtAttr {
    SpuVolume volume;
    s32 reverb;
    s32 mix;
} SpuExtAttr;

/* SpuSetCommonAttr: `mask` the attributes to set (SPU_COMMON_*). */
typedef struct SpuCommonAttr {
    u32 mask;
    SpuVolume mvol;
    SpuVolume mvolmode;
    SpuVolume mvolx;
    SpuExtAttr cd;
    SpuExtAttr ext;
} SpuCommonAttr;

#define SPU_VOICE_VOLL (1u << 0)
#define SPU_VOICE_VOLR (1u << 1)
#define SPU_VOICE_VOLMODEL (1u << 2)
#define SPU_VOICE_VOLMODER (1u << 3)
#define SPU_VOICE_PITCH (1u << 4)
#define SPU_VOICE_NOTE (1u << 5)
#define SPU_VOICE_SAMPLE_NOTE (1u << 6)
#define SPU_VOICE_WDSA (1u << 7)
#define SPU_VOICE_ADSR_AMODE (1u << 8)
#define SPU_VOICE_ADSR_SMODE (1u << 9)
#define SPU_VOICE_ADSR_RMODE (1u << 10)
#define SPU_VOICE_ADSR_AR (1u << 11)
#define SPU_VOICE_ADSR_DR (1u << 12)
#define SPU_VOICE_ADSR_SR (1u << 13)
#define SPU_VOICE_ADSR_RR (1u << 14)
#define SPU_VOICE_ADSR_SL (1u << 15)
#define SPU_VOICE_LSAX (1u << 16)
#define SPU_VOICE_ADSR_ADSR1 (1u << 17)
#define SPU_VOICE_ADSR_ADSR2 (1u << 18)

#define SPU_COMMON_MVOLL (1u << 0)
#define SPU_COMMON_MVOLR (1u << 1)
#define SPU_COMMON_MVOLMODEL (1u << 2)
#define SPU_COMMON_MVOLMODER (1u << 3)
#define SPU_COMMON_RVOLL (1u << 4)
#define SPU_COMMON_RVOLR (1u << 5)
#define SPU_COMMON_CDVOLL (1u << 6)
#define SPU_COMMON_CDVOLR (1u << 7)
#define SPU_COMMON_CDREV (1u << 8)
#define SPU_COMMON_CDMIX (1u << 9)
#define SPU_COMMON_EXTVOLL (1u << 10)
#define SPU_COMMON_EXTVOLR (1u << 11)
#define SPU_COMMON_EXTREV (1u << 12)
#define SPU_COMMON_EXTMIX (1u << 13)

void SpuSetVoiceAttr(SpuVoiceAttr *arg);
void SpuSetCommonAttr(SpuCommonAttr *attr);
s32 SpuClearReverbWorkArea(s32 rev_mode);

#endif /* PSYQ_LIBSPU_H */
