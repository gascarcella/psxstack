#ifndef PSYQ_LIBPAD_H
#define PSYQ_LIBPAD_H

/* psxstack's declarations of the Psy-Q 4.7 LIBPAD interface, as its games use it: written from the games' use of the
 * API and public documentation, no Sony header (DECISIONS "Psy-Q declarations: the stack's"). The shim implements
 * these; a game's own recovered declarations must agree with them in ABI (tools/psyq_decls.py). */

#include "psxstack/types.h"

void PadInitDirect(u8 *, u8 *);
void PadInitMtap(u8 *, u8 *);
int PadStartCom(void);
void PadStopCom(void);
int PadChkVsync(void);
int PadGetState(int port);
int PadInfoMode(int port, int term, int offs);
int PadInfoAct(int port, int actno, int term);
void PadSetAct(int port, u8 *data, int len);
int PadSetActAlign(int port, u8 *data);
int PadSetMainMode(int socket, int offs, int lock);

#endif /* PSYQ_LIBPAD_H */
