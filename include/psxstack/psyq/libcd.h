#ifndef PSYQ_LIBCD_H
#define PSYQ_LIBCD_H

/* psxstack's declarations of the Psy-Q 4.7 LIBCD interface, as its games use it: written from the games' use of the
 * API and public documentation, no Sony header (DECISIONS "Psy-Q declarations: the stack's"). The shim implements
 * these; a game's own recovered declarations must agree with them in ABI (tools/psyq_decls.py). */

#include "psxstack/types.h"

/* CD position as minute/second/sector (BCD) + track. */
typedef struct CdlLOC {
    u8 minute;
    u8 second;
    u8 sector;
    u8 track;
} CdlLOC;

/* Converts a sector number to a CdlLOC and returns `p`. (Its byte-identical LIBDS twin
 * DsIntToPos isn't linked: docs/TOOLCHAIN.md, "Ambiguous objects".) */
CdlLOC *CdIntToPos(int i, CdlLOC *p);
int CdPosToInt(CdlLOC *p);

int CdGetSector(void *madr, int size);
void *CdReadyCallback(void (*func)());
void *CdSyncCallback(void (*func)());
int CdInit(void);
int CdSetDebug(int level);
int CdControl(u8 com, u8 *param, u8 *result);
int CdControlB(u8 com, u8 *param, u8 *result);
int CdControlF(u8 com, u8 *param);

/* Streaming (LIBCD's st_*.c objects). */
int CdRead2(long mode);
void StSetRing(u32 *ring_addr, u32 ring_size);
void StSetStream(u32 mode, u32 start_frame, u32 end_frame, void (*func1)(), void (*func2)());
u32 StGetNext(u32 **addr, u32 **header);
u32 StFreeRing(u32 *base);
void StUnSetRing(void);
void StCdInterrupt(void);
extern u8 StCdIntrFlag; /* in LIBCD's .bss (one block): a movie player polls and clears it */

#endif /* PSYQ_LIBCD_H */
