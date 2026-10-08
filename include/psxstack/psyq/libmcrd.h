#ifndef PSYQ_LIBMCRD_H
#define PSYQ_LIBMCRD_H

/* psxstack's declarations of the Psy-Q 4.7 LIBMCRD interface, as its games use it: written from the games' use of the
 * API and public documentation, no Sony header (DECISIONS "Psy-Q declarations: the stack's"). The shim implements
 * these; a game's own recovered declarations must agree with them in ABI (tools/psyq_decls.py). */

#include "psxstack/types.h"

/* A memory card directory entry (MemCardGetDirentry; Sony declares it in KERNEL.H). */
typedef struct DIRENTRY {
    /* 0x00 */ char name[20];
    /* 0x14 */ s32 attr;
    /* 0x18 */ s32 size;
    /* 0x1C */ struct DIRENTRY *next;
    /* 0x20 */ s32 head;
    /* 0x24 */ char system[4];
} DIRENTRY; /* size 0x28 */

void MemCardInit(long val);
void MemCardStart(void);
s32 MemCardExist(s32 chan);
s32 MemCardAccept(s32 chan);
s32 MemCardReadFile(s32 chan, char *file, u32 *addr, s32 offset, s32 bytes);
s32 MemCardWriteFile(s32 chan, char *file, u32 *addr, s32 offset, s32 bytes);
s32 MemCardCreateFile(s32 chan, char *file, s32 blocks);
s32 MemCardFormat(s32 chan);
s32 MemCardUnformat(s32 chan);
s32 MemCardSync(s32 mode, s32 *cmds, s32 *result);
s32 MemCardGetDirentry(s32 chan, char *name, DIRENTRY *dir, s32 *files, s32 offset, s32 max);

#endif /* PSYQ_LIBMCRD_H */
