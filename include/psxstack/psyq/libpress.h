#ifndef PSYQ_LIBPRESS_H
#define PSYQ_LIBPRESS_H

/* psxstack's declarations of the Psy-Q 4.7 LIBPRESS interface, as its games use it: written from the games' use of the
 * API and public documentation, no Sony header (DECISIONS "Psy-Q declarations: the stack's"). The shim implements
 * these; a game's own recovered declarations must agree with them in ABI (tools/psyq_decls.py). */

#include "psxstack/types.h"

/* The header of a movie sector (the .STR format, StGetNext's `header`). */
typedef struct StHEADER {
    /* 0x00 */ u16 id;
    /* 0x02 */ u16 type;
    /* 0x04 */ u16 secCount;
    /* 0x06 */ u16 nSectors;
    /* 0x08 */ u32 frameCount;
    /* 0x0C */ u32 frameSize;
    /* 0x10 */ u16 width;
    /* 0x12 */ u16 height;
    /* 0x14 */ u32 headm;
    /* 0x18 */ u32 headv;
    /* 0x1C */ u32 *user;
} StHEADER; /* size 0x20 */

void DecDCTReset(int mode);
void DecDCTin(u32 *buf, int mode);
void DecDCTout(u32 *buf, int size);
void DecDCToutCallback(void (*func)());
int DecDCTvlc2(u32 *bs, u32 *buf, u16 *table);
void DecDCTvlcBuild(u16 *table);

#endif /* PSYQ_LIBPRESS_H */
