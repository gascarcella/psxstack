#ifndef PSYQ_LIBAPI_H
#define PSYQ_LIBAPI_H

/* psxstack's declarations of the Psy-Q 4.7 LIBAPI interface, as its games use it: written from the games' use of the
 * API and public documentation, no Sony header (DECISIONS "Psy-Q declarations: the stack's"). The shim implements
 * these (psyq/libapi.c, psyq/libcard.c); a game's own recovered declarations must agree with them in ABI
 * (tools/psyq_decls.py). Sony's `long` is the PS1's 32 bits: s32 here.
 *
 * open, read, write, lseek and close are also the host libc's names, EnterCriticalSection and ExitCriticalSection
 * Win32's: a game's units are compiled with <psxstack/psyq_names.h> forced in, which sends their calls to the shim's
 * psyq_api_* (docs/PORT.md "The Psy-Q shim"); the shim's psyq/libapi.c and libcard.c include it too, so these
 * prototypes and the definitions are renamed alike. */

#include "psxstack/types.h"
/* DIRENTRY (firstfile, nextfile; Sony declares it in KERNEL.H): the first game declares it with LIBMCRD, under the
 * same guard as the stack's libmcrd.h, so a unit that includes both games' kind of header sees one definition. */
#include "psxstack/psyq/libmcrd.h"

/* Event descriptors (classes): the root counters, the memory card's hardware and software events. */
#define RCntCNT0 0xF2000000u
#define RCntCNT1 0xF2000001u
#define RCntCNT2 0xF2000002u
#define RCntCNT3 0xF2000003u /* the vertical blank */
#define HwCARD 0xF0000011u
#define SwCARD 0xF4000001u

/* Event specs, modes and statuses. */
#define EvSpINT 0x0002
#define EvSpIOE 0x0004
#define EvSpTIMOUT 0x0100
#define EvSpNEW 0x2000
#define EvSpERROR 0x8000
#define EvMdINTR 0x1000
#define EvMdNOINTR 0x2000
#define EvStUNUSED 0x0000
#define EvStWAIT 0x1000
#define EvStACTIVE 0x2000
#define EvStALREADY 0x4000

/* Root counter modes (SetRCnt). */
#define RCntMdINTR 0x1000
#define RCntMdNOINTR 0x2000

/* open()'s flags (FCREAT's block count in the high 16 bits). Guarded: some host headers have FASYNC. */
#ifndef FREAD
#define FREAD 0x0001
#endif
#ifndef FWRITE
#define FWRITE 0x0002
#endif
#ifndef FNBLOCK
#define FNBLOCK 0x0004
#endif
#ifndef FCREAT
#define FCREAT 0x0200
#endif
#ifndef FASYNC
#define FASYNC 0x8000
#endif

/* Events. */
s32 OpenEvent(u32 desc, s32 spec, s32 mode, s32 (*func)());
s32 CloseEvent(s32 event);
s32 EnableEvent(s32 event);
s32 DisableEvent(s32 event);
s32 TestEvent(s32 event);
s32 WaitEvent(s32 event);

/* Root counters. */
s32 SetRCnt(u32 spec, u16 target, s32 mode);
s32 GetRCnt(u32 spec);
s32 StartRCnt(u32 spec);
s32 StopRCnt(u32 spec);
s32 ResetRCnt(u32 spec);

/* Interrupts. */
s32 EnterCriticalSection(void);
void ExitCriticalSection(void);
void ChangeClearPad(s32 val);

/* The BIOS's file calls. */
s32 open(char *name, s32 mode);
s32 read(s32 fd, void *buf, s32 n);
s32 write(s32 fd, void *buf, s32 n);
s32 lseek(s32 fd, s32 offset, s32 whence);
s32 close(s32 fd);
DIRENTRY *firstfile(char *name, DIRENTRY *dir);
DIRENTRY *nextfile(DIRENTRY *dir);
s32 erase(char *name);
s32 format(char *name);

/* The memory card (LIBCARD, which Sony declares in LIBAPI.H). */
void InitCARD(s32 val);
s32 StartCARD(void);
s32 StopCARD(void);
void _bu_init(void);
s32 _card_info(s32 chan);
s32 _card_load(s32 chan);
s32 _card_clear(s32 chan);
s32 _card_format(s32 chan);
s32 _card_status(s32 drv);
s32 _card_wait(s32 drv);
u32 _card_chan(void);

#endif /* PSYQ_LIBAPI_H */
